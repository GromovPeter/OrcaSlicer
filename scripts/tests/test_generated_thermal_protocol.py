"""Generated calibration protocol/recorder tests; no printer connections."""
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import contextlib
import hashlib
import io
import json
import math
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).parents[1]))
import generated_thermal_protocol as protocol
import run_loaded_thermal_calibration as worker


def example():
    sequence=[(1,250),(1,240),(1,250),(1,260),(5,260),(5,250),(5,260),(10,260),(5,260)]
    return dict(version=1,kind='orca_loaded_thermal',nominal_flow=5,filament_diameter=2.85,
        fan=.35,warmup_layers=7,measurement_z=2.1,height=60,total_layers=200,
        stage_timeout_s=120,total_timeout_s=1500,temperature_min=240,temperature_max=260,
        stages=[dict(name='stage_'+str(i),flow=q,target=t) for i,(q,t) in enumerate(sequence)])


class Clock:
    def __init__(self): self.now=0.0
    def monotonic(self): return self.now
    def sleep(self,seconds): self.now+=seconds


class GeneratedProtocolTests(unittest.TestCase):
    def fixture(self,directory,p=None):
        file=Path(directory)/'my-printer.gcode'
        file.write_bytes(protocol.MARKER+json.dumps(p or example()).encode()+b'\nG1 X5 E1\n')
        return file

    def test_local_file_carries_protocol_and_exact_checksum(self):
        with tempfile.TemporaryDirectory() as directory:
            file=self.fixture(directory);p=protocol.load_fixture(file)
            self.assertEqual(p['filament_diameter'],2.85)
            self.assertEqual(p['sha256'],hashlib.sha256(file.read_bytes()).hexdigest())
            self.assertEqual(protocol.stage_commands(protocol.stages(p)[0],p),'M104 S250\nM220 S20')

    def test_invalid_or_unsafe_stage_sequence_is_rejected(self):
        for change in ('cold_high_flow','nan','duplicate','wrong_nominal'):
            p=example()
            if change=='cold_high_flow':p['stages'][7]['target']=240
            if change=='nan':p['fan']=float('nan')
            if change=='duplicate':p['stages'][1]['name']=p['stages'][0]['name']
            if change=='wrong_nominal':p['nominal_flow']=10
            with self.subTest(change=change),self.assertRaises(ValueError):protocol.validate_protocol(p)

    def test_standard_fan_and_creality_pins_are_discovered_without_requiring_extra_channels(self):
        objects,part=protocol.cooling_objects(['fan','extruder','fan_generic chamber'])
        self.assertEqual(part,'fan')
        self.assertEqual(protocol.fan_values({'fan':{'speed':.35},'fan_generic chamber':{'speed':0}},objects),
                         {'fan':.35,'fan_generic chamber':0})
        objects,part=protocol.cooling_objects(['output_pin fan0','output_pin fan0_en'])
        self.assertEqual(part,'output_pin fan0')
        with self.assertRaises(RuntimeError):protocol.fan_values({},objects)

    def test_stock_virtual_sd_does_not_need_creality_layer_fields(self):
        s={'print_stats':{'info':{'current_layer':8}},'virtual_sdcard':{'is_active':True},
           'motion_report':{'live_position':[100,100,2.4]}}
        self.assertTrue(protocol.first_layers_ready(s,5,example()))
        s['print_stats']['info']['current_layer']=0
        self.assertFalse(protocol.first_layers_ready(s,5,example()))

    def test_modified_uploaded_file_fails_before_any_control(self):
        with tempfile.TemporaryDirectory() as directory:
            args=self.args(directory,self.fixture(directory))
            with patch.object(worker,'urlopen',return_value=io.BytesIO(b'changed file')),\
                 patch.object(worker,'post') as post,patch.object(worker,'query') as query,\
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(worker.execute(args),1)
                post.assert_not_called();query.assert_not_called()
            self.assertFalse(Path(args.output).exists())

    def args(self,directory,file):
        return SimpleNamespace(url='http://mock.invalid',filename='folder/'+file.name,fixture=str(file),
            output=str(Path(directory)/'test.csv'),filament_diameter=1.75,
            stop_file=None,heartbeat_file=None,progress_json=str(Path(directory)/'progress.json'),verify_upload=True)

    def simulate(self,directory,replace_at=None,fan_change_at=None):
        file=self.fixture(directory);args=self.args(directory,file);clock=Clock()
        state={'target':250.0,'flow':5.0,'cancelled':False};posts=[]
        def query(*unused):
            filename='ordinary.gcode' if replace_at is not None and clock.now>=replace_at else args.filename
            if state['cancelled']:
                return {'status':{'print_stats':{'state':'cancelled','filename':filename},
                    'virtual_sdcard':{'is_active':False},'extruder':{'target':0},
                    'heater_bed':{'target':0},'gcode_move':{'speed_factor':1}}}
            ready=clock.now>=3
            return {'eventtime':clock.now,'status':{
                'print_stats':{'state':'printing','filename':filename,'info':{'current_layer':8 if ready else 1}},
                'virtual_sdcard':{'is_active':True},
                'motion_report':{'live_position':[100,100,2.4 if ready else .3],
                    'live_extruder_velocity':state['flow']/(math.pi*(2.85/2)**2)},
                'extruder':{'temperature':state['target'],'target':state['target'],'power':.3},
                'heater_bed':{'temperature':100,'target':100},
                'gcode_move':{'extrude_factor':1,'speed_factor':state['flow']/5},
                'fan':{'speed':.5 if fan_change_at is not None and clock.now>=fan_change_at else .35}}}
        def post(base,path,body,key):
            posts.append((clock.now,path,body))
            if path=='/printer/print/cancel':state['cancelled']=True
            for command in body.get('script','').splitlines():
                if command.startswith('M104 S'):state['target']=float(command[6:])
                if command.startswith('M220 S'):state['flow']=5*float(command[6:])/100
        with patch.object(worker,'urlopen',return_value=io.BytesIO(file.read_bytes())),\
             patch.object(worker,'read_json',return_value={'objects':['fan','extruder','heater_bed']}),\
             patch.object(worker,'query',side_effect=query),patch.object(worker,'post',side_effect=post),\
             patch.object(worker.time,'monotonic',clock.monotonic),patch.object(worker.time,'sleep',clock.sleep),\
             contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
            code=worker.execute(args)
        return code,posts,json.loads(Path(args.output+'.summary.json').read_text())

    def test_generic_printer_completes_nine_stages_and_confirms_cancel(self):
        with tempfile.TemporaryDirectory() as directory:
            code,posts,summary=self.simulate(directory)
            self.assertEqual(code,0)
            self.assertEqual(len(summary['stages']),9)
            self.assertTrue(summary['cancellation_confirmed'])
            self.assertEqual(summary['generated_protocol']['filament_diameter'],2.85)
            self.assertEqual(sum(path=='/printer/print/cancel' for t,path,b in posts),1)
            self.assertTrue(all(t>=3 for t,path,b in posts if b.get('script','').startswith('M104')))

    def test_generic_printer_fan_change_aborts_and_cancels(self):
        with tempfile.TemporaryDirectory() as directory:
            code,posts,summary=self.simulate(directory,fan_change_at=20)
            self.assertEqual(code,1);self.assertFalse(summary['completed'])
            self.assertTrue(summary['cancellation_confirmed'])

    def test_replacement_job_is_not_cancelled_with_generated_protocol(self):
        with tempfile.TemporaryDirectory() as directory:
            code,posts,summary=self.simulate(directory,replace_at=20)
            self.assertEqual(code,1);self.assertFalse(summary['cancellation_confirmed'])
            self.assertFalse(any(path=='/printer/print/cancel' for t,path,b in posts))

if __name__=='__main__':unittest.main()
