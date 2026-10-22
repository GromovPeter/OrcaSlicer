import importlib.util
from pathlib import Path
import sys
import unittest

spec=importlib.util.spec_from_file_location('loaded_calibration',Path(__file__).parents[1]/'loaded_thermal_calibration.py')
m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m)


class LoadedCalibrationTests(unittest.TestCase):
    def test_stable_printing_advances_without_a_fixed_two_minute_wait(self):
        c=m.LoadedCalibration();c.start(0)
        next_stage=None
        for t in range(16):
            next_stage=c.observe(t,213,213,1)
        self.assertEqual(next_stage.name,'cool_at_1')
        self.assertEqual(c.results[0]['elapsed_s'],15)

    def test_hot_but_stationary_nozzle_does_not_qualify(self):
        c=m.LoadedCalibration();c.start(0)
        for t in range(30): self.assertIsNone(c.observe(t,213,213,0))
        self.assertEqual(c.index,0)

    def test_temperature_excursion_resets_the_continuous_dwell(self):
        c=m.LoadedCalibration();c.start(0)
        for t in range(15): c.observe(t,213,213,1)
        self.assertIsNone(c.observe(15,215,213,1))
        for t in range(16,26): self.assertIsNone(c.observe(t,213,213,1))
        self.assertEqual(c.observe(26,213,213,1).name,'cool_at_1')

    def test_timeouts_fail_instead_of_reporting_false_calibration(self):
        c=m.LoadedCalibration(timeout_s=20);c.start(0)
        for t in range(20): c.observe(t,210,213,1)
        with self.assertRaises(TimeoutError):c.observe(20,210,213,1)

    def test_clock_gaps_and_changed_targets_fail_explicitly(self):
        c=m.LoadedCalibration();c.start(0);c.observe(0,213,213,1)
        with self.assertRaises(RuntimeError):c.observe(3,213,213,1)
        c=m.LoadedCalibration();c.start(0)
        with self.assertRaises(RuntimeError):c.observe(0,213,220,1)

    def test_all_stages_finish_only_after_observed_extrusion_and_settling(self):
        c=m.LoadedCalibration();c.start(0);t=0
        while not c.done:
            s=c.stage;c.observe(t,s.target,s.target,s.flow);t+=1
        self.assertEqual(len(c.results),9)
        self.assertLess(t,180)

    def test_high_flow_never_requests_the_low_temperature(self):
        for s in m.PLA_STAGES:
            minimum=196 if s.flow==1 else 213 if s.flow==10 else 228
            self.assertGreaterEqual(s.target,minimum)


class LoadedRunnerTests(unittest.TestCase):
    def runner(self):
        scripts=str(Path(__file__).parents[1]);sys.path.insert(0,scripts)
        import run_loaded_thermal_calibration
        return run_loaded_thermal_calibration

    def test_other_job_and_paused_job_are_rejected(self):
        r=self.runner()
        for state,name in [('printing','ordinary-print.gcode'),('paused',r.FILENAME),('complete',r.FILENAME)]:
            with self.assertRaises(RuntimeError):r.require_calibration_job({'print_stats':{'state':state,'filename':name}},r.FILENAME)
        r.require_calibration_job({'print_stats':{'state':'printing','filename':r.FILENAME}},r.FILENAME)

    def test_control_commands_are_only_target_and_speed_override(self):
        r=self.runner()
        for stage in r.PLA_STAGES:
            lines=r.stage_commands(stage).splitlines()
            self.assertEqual([x.split()[0] for x in lines],['M104','M220'])
        with self.assertRaises(ValueError):r.stage_commands(m.Stage('wrong',22,196))

    def cancelled(self, r):
        return {'print_stats':{'state':'cancelled','filename':r.FILENAME},
                'virtual_sdcard':{'is_active':False},'extruder':{'target':0},
                'heater_bed':{'target':0},'gcode_move':{'speed_factor':1}}

    def test_cancel_is_attempted_even_if_speed_restore_fails(self):
        from unittest.mock import patch
        r=self.runner();active={'print_stats':{'state':'printing','filename':r.FILENAME}}
        with patch.object(r,'query',side_effect=[{'status':active},{'status':self.cancelled(r)}]),patch.object(r,'post',side_effect=[RuntimeError('restore failed'),None]) as post:
            r.stop_calibration('http://localhost',r.FILENAME,None)
            self.assertEqual(post.call_args_list[-1].args[1],'/printer/print/cancel')

    def test_slow_cancel_is_confirmed_without_repeating_the_command(self):
        from unittest.mock import patch
        r=self.runner();active={'print_stats':{'state':'printing','filename':r.FILENAME}}
        with patch.object(r,'query',side_effect=[{'status':active},{'status':active},{'status':self.cancelled(r)}]),patch.object(r,'post',side_effect=[None,TimeoutError()]) as post,patch.object(r.time,'sleep'):
            r.stop_calibration('http://localhost',r.FILENAME,None)
            self.assertEqual(post.call_count,2)

    def test_cancel_timeout_and_replacement_job_are_not_reported_as_success(self):
        from unittest.mock import patch
        r=self.runner();active={'print_stats':{'state':'printing','filename':r.FILENAME}}
        with patch.object(r,'query',return_value={'status':active}),patch.object(r,'post'):
            with self.assertRaises(TimeoutError):r.stop_calibration('http://localhost',r.FILENAME,None,confirmation_timeout_s=0)
        other=self.cancelled(r);other['print_stats']['filename']='other.gcode'
        with patch.object(r,'query',side_effect=[{'status':active},{'status':other}]),patch.object(r,'post') as post:
            with self.assertRaises(RuntimeError):r.stop_calibration('http://localhost',r.FILENAME,None)
            self.assertEqual(post.call_count,2)

    def test_cancelled_with_heater_on_or_speed_override_is_not_confirmed(self):
        r=self.runner()
        s=self.cancelled(r);s['extruder']['target']=228
        self.assertFalse(r.cancellation_confirmed(s,r.FILENAME))
        s=self.cancelled(r);s['gcode_move']['speed_factor']=2.2
        self.assertFalse(r.cancellation_confirmed(s,r.FILENAME))

    def test_layer_initialization_preserves_the_synchronized_current_layer_marker(self):
        from unittest.mock import patch
        r=self.runner();s={'print_stats':{'state':'printing','filename':r.FILENAME},'virtual_sdcard':{'is_active':True,'layer_count':150}}
        with patch.object(r,'post') as post:
            self.assertTrue(r.initialize_layer_statistics('http://localhost',r.FILENAME,None,s))
            self.assertEqual(post.call_args.args[2]['script'],'SET_PRINT_STATS_INFO TOTAL_LAYER=150')
            s['virtual_sdcard']['layer_count']=None
            self.assertFalse(r.initialize_layer_statistics('http://localhost',r.FILENAME,None,s))
            self.assertEqual(post.call_count,1)


class SensorFitTests(unittest.TestCase):
    def test_ramp_recovers_rate_and_delay(self):
        points=[(t,min(228,213+max(0,t-2)/2)) for t in range(45)]
        fit=m.fit_sensor_step(points,213,228)
        self.assertAlmostEqual(fit['response_seconds_per_degree'],2)
        self.assertAlmostEqual(fit['extrapolated_delay_s'],2)

    def test_load_change_is_not_fitted_as_a_target_step(self):
        self.assertEqual(m.fit_sensor_step([(0,228),(10,227),(20,228)],228,228),{})


class LoadQualificationTests(unittest.TestCase):
    def test_interrupted_load_is_excluded_from_temperature_response_fit(self):
        c=m.LoadedCalibration();c.start(0)
        for t in range(16):c.observe(t,213,213,1)
        for t in range(16,48):
            result=c.observe(t,196,196,0 if t==17 else 1)
            if result is not None:break
        self.assertEqual(c.results[-1]['stage'],'cool_at_1')
        self.assertTrue(c.results[-1]['load_disrupted_during_stage'])
        self.assertNotIn('response_seconds_per_degree',c.results[-1])
        self.assertIn('response_excluded',c.results[-1])


class FirstLayerGuardTests(unittest.TestCase):
    def test_a_high_z_startup_purge_is_not_a_completed_first_layer(self):
        sys.path.insert(0,str(Path(__file__).parents[1]));import run_loaded_thermal_calibration as r
        s={'motion_report':{'live_position':[0,0,10]},'print_stats':{'info':{'current_layer':0}},'virtual_sdcard':{'layer':7}}
        self.assertFalse(r.first_layers_ready(s,10,{'fan0':1}))
        s['print_stats']['info']['current_layer']=7
        s['motion_report']['live_position'][2]=.2
        self.assertFalse(r.first_layers_ready(s,10,{'fan0':1}))
        s['motion_report']['live_position'][2]=2
        self.assertTrue(r.first_layers_ready(s,10,{'fan0':1}))
        s['virtual_sdcard']['layer']=0
        self.assertFalse(r.first_layers_ready(s,10,{'fan0':1}))

if __name__=='__main__':unittest.main()
