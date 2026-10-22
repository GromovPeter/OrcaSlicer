"""GUI worker lifecycle tests; every HTTP operation is mocked."""
from pathlib import Path
from types import SimpleNamespace
import contextlib
import io
import json
import math
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import run_loaded_thermal_calibration as worker


class Clock:
    def __init__(self): self.now = 0.0
    def monotonic(self): return self.now
    def sleep(self, seconds): self.now += seconds


class GuiWorkerTests(unittest.TestCase):
    def args(self, directory):
        return SimpleNamespace(url='http://mock.invalid', filename=worker.FILENAME,
            output=str(Path(directory)/'response.csv'), filament_diameter=1.75,
            stop_file=None, heartbeat_file=None, progress_json=str(Path(directory)/'progress.json'),
            verify_upload=False)

    def simulate(self, args, clock, stop_at=None, replacement_at=None, broken_cancel=False):
        state={'target':213, 'flow':10, 'cancelled':False}
        posts=[]
        def query(*unused):
            if stop_at is not None and clock.now >= stop_at:
                Path(args.stop_file).write_text('stop')
            name='ordinary.gcode' if replacement_at is not None and clock.now >= replacement_at else args.filename
            if state['cancelled']:
                return {'status':{'print_stats':{'state':'cancelled','filename':name},
                    'virtual_sdcard':{'is_active':False},'extruder':{'target':0},
                    'heater_bed':{'target':0},'gcode_move':{'speed_factor':1}}}
            layer=7 if clock.now >= 3 else 1
            s={'print_stats':{'state':'printing','filename':name,'info':{'current_layer':layer}},
                'virtual_sdcard':{'is_active':True,'layer':layer,'layer_count':150},
                'motion_report':{'live_position':[0,0,2.5 if layer==7 else .2],
                    'live_extruder_velocity':state['flow']/(math.pi*(1.75/2)**2)},
                'extruder':{'temperature':state['target'],'target':state['target'],'power':.5},
                'heater_bed':{'temperature':50,'target':50},
                'gcode_move':{'extrude_factor':1,'speed_factor':state['flow']/10}}
            for pin,value in (('fan0',1),('fan0_en',1),('fan1',0),('fan2',0)):
                s['output_pin '+pin]={'value':value}
            return {'eventtime':clock.now,'status':s}
        def post(base,path,body,key):
            posts.append((clock.now,path,body))
            if path=='/printer/print/cancel':
                if broken_cancel: raise RuntimeError('simulated cancellation rejected')
                state['cancelled']=True
            else:
                for line in body['script'].splitlines():
                    if line.startswith('M104 S'):state['target']=float(line[6:])
                    if line.startswith('M220 S'):state['flow']=float(line[6:])/10
        with patch.object(worker,'query',side_effect=query),patch.object(worker,'post',side_effect=post),\
             patch.object(worker.time,'monotonic',clock.monotonic),patch.object(worker.time,'sleep',clock.sleep),\
             contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
            code=worker.execute(args)
        return code,posts,json.loads(Path(args.progress_json).read_text())

    def test_complete_run_checks_all_stages_and_cancellation_before_success(self):
        with tempfile.TemporaryDirectory() as directory:
            args=self.args(directory);clock=Clock()
            code,posts,progress=self.simulate(args,clock)
            self.assertEqual(code,0)
            self.assertEqual(progress['state'],'completed')
            self.assertTrue(progress['cleanup_confirmed'])
            summary=json.loads(Path(args.output+'.summary.json').read_text())
            self.assertEqual(len(summary['stages']),9)
            self.assertTrue(summary['completed'])
            self.assertEqual(sum(p[1]=='/printer/print/cancel' for p in posts),1)
            self.assertTrue(all(t>=3 for t,path,body in posts if body.get('script','').startswith('M104')))
            self.assertGreater(clock.now,130)
            self.assertLess(clock.now,200)

    def test_gui_stop_retains_partial_csv_and_confirms_guarded_cancellation(self):
        with tempfile.TemporaryDirectory() as directory:
            args=self.args(directory);args.stop_file=str(Path(directory)/'stop')
            code,posts,progress=self.simulate(args,Clock(),stop_at=20)
            self.assertEqual(code,2)
            self.assertEqual(progress['state'],'stopped')
            self.assertTrue(progress['cleanup_confirmed'])
            self.assertFalse(json.loads(Path(args.output+'.summary.json').read_text())['completed'])
            self.assertTrue(Path(args.output).exists())
            self.assertEqual(sum(p[1]=='/printer/print/cancel' for p in posts),1)

    def test_replacement_job_is_never_cancelled(self):
        with tempfile.TemporaryDirectory() as directory:
            code,posts,progress=self.simulate(self.args(directory),Clock(),replacement_at=20)
            self.assertEqual(code,1)
            self.assertEqual(progress['state'],'error')
            self.assertFalse(progress['cleanup_confirmed'])
            self.assertFalse(any(p[1]=='/printer/print/cancel' for p in posts))

    def test_failed_cancellation_does_not_display_successful_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            args=self.args(directory);args.stop_file=str(Path(directory)/'stop')
            code,posts,progress=self.simulate(args,Clock(),stop_at=20,broken_cancel=True)
            self.assertEqual(code,2)
            self.assertEqual(progress['state'],'error')
            self.assertFalse(progress['cleanup_confirmed'])

    def test_owner_crash_causes_cleanup_without_hard_killing_the_worker(self):
        with tempfile.TemporaryDirectory() as directory:
            args=self.args(directory);args.heartbeat_file=str(Path(directory)/'heartbeat')
            Path(args.heartbeat_file).write_text('0')
            code,posts,progress=self.simulate(args,Clock())
            self.assertEqual(code,2)
            self.assertEqual(progress['state'],'stopped')
            self.assertTrue(progress['cleanup_confirmed'])
            self.assertLess(posts[-1][0],17)

    def test_refreshing_heartbeat_keeps_owner_alive(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'heartbeat';path.write_text('0');clock=Clock()
            with patch.object(worker.time,'monotonic',clock.monotonic):
                guard=worker.OwnerGuard(heartbeat_file=path);guard.check()
                for i in range(1,10):
                    clock.sleep(10);path.write_text(str(i));guard.check()
                clock.sleep(16)
                with self.assertRaises(worker.CalibrationStopped):guard.check()

    def test_upload_hash_is_checked_not_just_the_filename(self):
        fixture=Path(__file__).parents[2]/'resources/calibration'/worker.FILENAME
        with patch.object(worker,'urlopen',return_value=io.BytesIO(fixture.read_bytes())) as request:
            worker.verify_uploaded_fixture('http://mock.invalid','folder/'+worker.FILENAME,None)
            self.assertIn('/server/files/gcodes/folder/',request.call_args.args[0].full_url)
        with patch.object(worker,'urlopen',return_value=io.BytesIO(b'G1 X5')):
            with self.assertRaises(RuntimeError):worker.verify_uploaded_fixture('http://mock.invalid',worker.FILENAME,None)

    def test_wrong_upload_fails_before_any_printer_control(self):
        with tempfile.TemporaryDirectory() as directory:
            args=self.args(directory);args.verify_upload=True
            with patch.object(worker,'urlopen',return_value=io.BytesIO(b'ordinary print')),\
                 patch.object(worker,'post') as post,patch.object(worker,'query') as query,\
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(worker.execute(args),1)
                post.assert_not_called();query.assert_not_called()
            self.assertFalse(Path(args.output).exists())

    def test_unwritable_progress_does_not_prevent_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            args=self.args(directory);args.stop_file=str(Path(directory)/'stop')
            with patch.object(worker.os,'replace',side_effect=PermissionError('progress locked')):
                # Simulate directly: no readable progress is expected; cleanup summary remains authoritative.
                clock=Clock()
                with self.assertRaises(FileNotFoundError):self.simulate(args,clock,stop_at=20)
            summary=json.loads(Path(args.output+'.summary.json').read_text())
            self.assertTrue(summary['cancellation_confirmed'])

    def test_existing_csv_is_never_overwritten_and_no_commands_are_sent(self):
        with tempfile.TemporaryDirectory() as directory:
            args=self.args(directory);Path(args.output).write_text('keep')
            with patch.object(worker,'post') as post,patch.object(worker,'query') as query,contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(worker.execute(args),1)
                post.assert_not_called();query.assert_not_called()
            self.assertEqual(Path(args.output).read_text(),'keep')


if __name__=='__main__':unittest.main()
