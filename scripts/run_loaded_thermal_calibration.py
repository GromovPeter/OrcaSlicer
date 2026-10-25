#!/usr/bin/env python3
"""Control ONLY a verified continuous spiral calibration print.
Requires explicit --execute. Does not upload or start a print or install macros.
"""
import argparse
import contextlib
import hashlib
import sys
import traceback
import csv
import json
import math
import os
from pathlib import Path, PurePosixPath
import time
from urllib.parse import quote, urlencode, urlsplit
from urllib.request import Request, urlopen
from record_hotend_response import read_json
from loaded_thermal_calibration import LoadedCalibration, PLA_STAGES
import generated_thermal_protocol as generated

FILENAME = 'ORCA_PLA_LOADED_CALIBRATION_20261021_v2.gcode'


def require_calibration_job(status, filename):
    job = status.get('print_stats', {})
    if job.get('state') != 'printing' or job.get('filename') != filename:
        raise RuntimeError('The expected calibration file is not actively printing')
    if status.get('pause_resume', {}).get('is_paused'):
        raise RuntimeError('Calibration print is paused')


def first_layers_ready(status, flow, pins):
    info=status.get('print_stats',{}).get('info') or {}
    return (int(info.get('current_layer') or 0)>=7 and
            int(status.get('virtual_sdcard',{}).get('layer') or 0)>=7 and
            2.0<=status['motion_report']['live_position'][2]<=40.0 and flow>0 and abs(pins['fan0']-1)<=.02)


def stage_commands(stage):
    minimum = 196 if stage.flow == 1 else 213 if stage.flow == 10 else 228
    if stage.flow not in (1, 10, 22) or stage.target < minimum or stage.target > 228:
        raise ValueError('Stage falls outside the supplied PLA flow/temperature table')
    return f'M104 S{stage.target:g}\nM220 S{stage.flow * 10:g}'


def post(base, path, body, key):
    headers = {'Content-Type': 'application/json'}
    if key: headers['X-Api-Key'] = key
    with urlopen(Request(base.rstrip('/') + path, data=json.dumps(body).encode(), headers=headers, method='POST'), timeout=5) as reply:
        document = json.load(reply)
    if 'error' in document: raise RuntimeError('Moonraker rejected the calibration command')


def query(base, key, cooling=None, has_bed=True):
    objects = {'print_stats':'state,filename,info','pause_resume':'is_paused','extruder':'temperature,target,power',
               'motion_report':'live_extruder_velocity,live_position','gcode_move':'extrude_factor,speed_factor',
               'heater_bed':'temperature,target','virtual_sdcard':'layer,layer_count,is_active'}
    if cooling is None:
        objects.update({'output_pin '+p:'value' for p in ('fan0','fan1','fan2','fan0_en')})
    else:
        objects['virtual_sdcard']='is_active'
        objects.update(cooling)
        if not has_bed:objects.pop("heater_bed",None)
    return read_json(base, '/printer/objects/query?' + urlencode(objects), key)


def initialize_layer_statistics(base, filename, key, status):
    # TOTAL_LAYER must be initialized on K2PRO before its per-layer markers
    # update current_layer. Do not synthesize CURRENT_LAYER: the fixture's
    # synchronized marker remains the authority for leaving initial layers.
    require_calibration_job(status, filename)
    sd = status.get('virtual_sdcard', {})
    total = sd.get('layer_count')
    if sd.get('is_active') is not True or not isinstance(total, int) or not 7 <= total <= 100000:
        return False
    post(base, '/printer/gcode/script', {'script': f'SET_PRINT_STATS_INFO TOTAL_LAYER={total}'}, key)
    return True


def cancellation_confirmed(status, filename):
    job = status.get('print_stats', {})
    return (job.get('filename') == filename and job.get('state') == 'cancelled' and
            status.get('virtual_sdcard', {}).get('is_active') is False and
            status.get('extruder', {}).get('target') == 0 and
            status.get('heater_bed', {}).get('target', 0) == 0 and
            abs(status.get('gcode_move', {}).get('speed_factor', -1) - 1) < .001)


def stop_calibration(base, filename, key, confirmation_timeout_s=90, query_fn=None):
    fetch = query if query_fn is None else query_fn
    # Recheck identity before canceling: never stop a replacement/ordinary job.
    current = fetch(base, key)['status']
    if cancellation_confirmed(current, filename):
        return current
    job = current.get('print_stats', {})
    if job.get('filename') != filename or job.get('state') not in ('printing', 'paused'):
        raise RuntimeError('Calibration job identity changed before cancellation')
    restore_error = None
    try:
        post(base, '/printer/gcode/script', {'script': 'M220 S100'}, key)
    except Exception as error:
        restore_error = error
    try:
        post(base, '/printer/print/cancel', {}, key)
    except (TimeoutError, OSError):
        # Creality cancellation can outlive the HTTP request. Poll the result,
        # without reissuing cancel or assuming a timeout means rejection.
        pass
    deadline = time.monotonic() + confirmation_timeout_s
    while True:
        try:
            current = fetch(base, key)['status']
        except (TimeoutError, OSError):
            current = None
        if current is not None:
            if current.get('print_stats', {}).get('filename') != filename:
                raise RuntimeError('Job identity changed while confirming cancellation')
            if cancellation_confirmed(current, filename):
                return current
        if time.monotonic() >= deadline:
            raise TimeoutError('Cancellation, heaters off and speed restoration could not be confirmed') from restore_error
        time.sleep(1)



FIXTURE_SHA256 = "92d9ff30a4daa5da7ee12f79970930026b798593638ad74d2e6966619e2242a7"

class CalibrationStopped(Exception):
    """Explicit GUI stop or expired owner heartbeat; cleanup still runs."""


class OwnerGuard:
    def __init__(self, stop_file=None, heartbeat_file=None):
        self.stop_file = Path(stop_file) if stop_file else None
        self.heartbeat_file = Path(heartbeat_file) if heartbeat_file else None
        self.last_value = None
        self.last_change = time.monotonic()

    def check(self):
        if self.stop_file and self.stop_file.exists():
            raise CalibrationStopped('Recording stopped by the user')
        if self.heartbeat_file:
            try:
                value = self.heartbeat_file.read_bytes()
            except OSError:
                value = None
            if value is not None and value != self.last_value:
                self.last_value = value
                self.last_change = time.monotonic()
            if time.monotonic() - self.last_change > 15:
                raise CalibrationStopped('Orca heartbeat expired')


def write_progress(path, state, **values):
    # Diagnostic IPC must never interrupt guarded printer cleanup.
    if path:
        try:
            output = Path(path)
            temporary = output.with_suffix(output.suffix + '.tmp')
            temporary.write_text(json.dumps(dict(state=state, **values), allow_nan=False), encoding='utf-8')
            os.replace(temporary, output)
        except OSError:
            pass


def verify_uploaded_fixture(base, filename, key, expected=None):
    headers = {'X-Api-Key': key} if key else {}
    with urlopen(Request(base.rstrip('/') + '/server/files/gcodes/' + quote(filename, safe='/'), headers=headers), timeout=5) as reply:
        if expected is not None:
            digest=hashlib.sha256();count=0
            while True:
                chunk=reply.read(1024*1024)
                if not chunk:break
                count+=len(chunk)
                if count>generated.MAX_FILE_BYTES:raise RuntimeError('Uploaded calibration file is too large')
                digest.update(chunk)
            if count != expected['bytes'] or digest.hexdigest() != expected['sha256']:
                raise RuntimeError('Uploaded calibration file differs from the locally generated file')
            return
        data = reply.read(2 * 1024 * 1024)
    if hashlib.sha256(data).hexdigest() != FIXTURE_SHA256:
        raise RuntimeError('Uploaded calibration file differs from the supplied fixture')


def run(args):
    guard=OwnerGuard(args.stop_file,args.heartbeat_file)
    guard.check()
    protocol=generated.load_fixture(args.fixture) if getattr(args,'fixture',None) else None
    if protocol is not None or args.verify_upload:
        if protocol is not None:
            verify_uploaded_fixture(args.url,args.filename,os.environ.get("MOONRAKER_API_KEY"),protocol)
        else:
            verify_uploaded_fixture(args.url,args.filename,os.environ.get("MOONRAKER_API_KEY"))
    if protocol is not None:
        names=read_json(args.url,'/printer/objects/list',os.environ.get('MOONRAKER_API_KEY'))['objects']
        protocol['cooling'],protocol['part_fan']=generated.cooling_objects(names)
        protocol['has_bed']='heater_bed' in names
        args.filament_diameter=protocol['filament_diameter']
    return run_measurement(args,guard,protocol)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--filename', default=FILENAME)
    parser.add_argument('--output', required=True, help='New raw CSV, never overwritten')
    parser.add_argument('--filament-diameter', type=float, default=1.75)
    parser.add_argument('--fixture', help='Local Orca-generated G-code used to verify the uploaded file and protocol')
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--stop-file')
    parser.add_argument('--heartbeat-file')
    parser.add_argument('--progress-json')
    parser.add_argument('--log-file')
    parser.add_argument('--verify-upload', action='store_true')
    args=parser.parse_args()
    if not args.execute: parser.error('Review the prepared print and pass --execute to control its temperatures and M220')
    if not args.fixture and PurePosixPath(args.filename).name != FILENAME: parser.error('Only the named calibration fixture is supported')
    if not math.isfinite(args.filament_diameter) or not 1 <= args.filament_diameter <= 3:
        parser.error('Invalid filament diameter')
    parsed=urlsplit(args.url)
    if parsed.scheme not in ('http','https') or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        parser.error('Use an HTTP(S) base URL without credentials')
    if Path(args.output+'.summary.json').exists():parser.error('Summary output already exists')
    if not args.fixture and abs(args.filament_diameter-1.75)>.001:parser.error('Prepared fixture uses 1.75 mm filament')
    if args.log_file:
        with open(args.log_file,'x',encoding='utf-8') as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            return execute(args)
    return execute(args)


def execute(args):
    try:
        run(args)
        return 0
    except CalibrationStopped:
        return 2
    except Exception:
        traceback.print_exc()
        try:
            progress=json.loads(Path(args.progress_json).read_text(encoding='utf-8')) if args.progress_json else {}
        except (OSError,ValueError):
            progress={}
        if progress.get('state') not in ('error','stopped','completed'):
            write_progress(args.progress_json,'error',cleanup_confirmed=False)
        return 1


def run_measurement(args,guard,protocol=None):
    fetch=(lambda base,key:query(base,key,protocol['cooling'],protocol['has_bed'])) if protocol else query
    command=(lambda stage:generated.stage_commands(stage,protocol)) if protocol else stage_commands
    def cancel():
        return stop_calibration(args.url,args.filename,key,query_fn=fetch) if protocol else stop_calibration(args.url,args.filename,key)
    cooling_baseline=None
    area=math.pi*(args.filament_diameter/2)**2
    key=os.environ.get('MOONRAKER_API_KEY')
    c=LoadedCalibration(timeout_s=protocol["stage_timeout_s"],stages=generated.stages(protocol)) if protocol else LoadedCalibration()
    first=None; activated=False; job_verified=False; waiting_target_since=None
    layers_initialized=False; cleanup_attempted=False; cleanup_status=None; cleanup_error=None
    write_progress(args.progress_json,'waiting')
    with open(args.output,'x',newline='',encoding='utf-8') as f:
        f.write('# Under-load calibration. requested_flow is derived from Klipper commanded extruder velocity, not measured melt flow.\n')
        f.write('# flow_mm3_s deliberately unset: raw varying commanded flow must not be accepted as constant deposition by the B2a importer.\n')
        cooling_columns=sorted(protocol['cooling']) if protocol else []
        if protocol:f.write('# generated_protocol='+json.dumps(protocol,allow_nan=False)+'\n')
        writer=csv.writer(f)
        writer.writerow(['time_s','target_c','temperature_c','power','fan','flow_mm3_s','requested_flow_mm3_s',
                         'pin_fan1','pin_fan2','bed_c','stage']+['cooling:'+name for name in cooling_columns])
        started=time.monotonic()
        try:
            while time.monotonic()-started < (protocol['total_timeout_s'] if protocol else 600):
                guard.check()
                received=fetch(args.url,key);s=received['status'];event=float(received['eventtime'])
                require_calibration_job(s,args.filename)
                job_verified=True
                motion=s['motion_report'];hot=s['extruder']
                if first is None:first=event
                if protocol:
                    cooling=generated.fan_values(s,protocol['cooling'])
                    pins={'fan0':cooling[protocol['part_fan']], 'fan0_en':1.0,
                          'fan1':cooling.get('output_pin fan1',cooling.get('fan_generic chamber',0.0)),
                          'fan2':cooling.get('output_pin fan2',cooling.get('fan_generic auxiliary',0.0))}
                else:
                    pins={p:s.get('output_pin '+p,{}).get('value') for p in ('fan0','fan1','fan2','fan0_en')}
                    if any(v is None for v in pins.values()):raise RuntimeError('Fan state unavailable')
                velocity=float(motion['live_extruder_velocity']);flow=velocity*area
                if not all(math.isfinite(v) for v in (event,flow,float(hot['temperature']),float(hot['target']))):
                    raise RuntimeError('Invalid telemetry')
                writer.writerow([event-first,hot['target'],hot['temperature'],hot.get('power',''),pins['fan0'],'',flow,
                                 pins['fan1'],pins['fan2'],s.get('heater_bed',{}).get('temperature',''),c.stage.name if c.stage else 'first_layers']+
                                [cooling[name] for name in cooling_columns])
                f.flush()
                if not activated:
                    if not layers_initialized:
                        if protocol:
                            post(args.url,'/printer/gcode/script',{'script':f'SET_PRINT_STATS_INFO TOTAL_LAYER={protocol["total_layers"]}'},key)
                            layers_initialized=True
                        else:
                            layers_initialized=initialize_layer_statistics(args.url,args.filename,key,s)
                    # Leave bottom, brim, initial cooling ramp and start macros untouched.
                    write_progress(args.progress_json,'waiting',temperature=float(hot['temperature']))
                    ready=generated.first_layers_ready(s,flow,protocol) if protocol else first_layers_ready(s,flow,pins)
                    if not ready:
                        time.sleep(.5);continue
                    if protocol:
                        if abs(pins['fan0']-protocol['fan'])>.02:time.sleep(.5);continue
                        if protocol['fan']>0 and 'output_pin fan0_en' in cooling and abs(cooling['output_pin fan0_en']-1)>.02:
                            raise RuntimeError('Part cooling output is disabled')
                        cooling_baseline=dict(cooling)
                        if abs(float(s['gcode_move']['extrude_factor'])-1)>.001:
                            raise RuntimeError('Runtime extrusion multiplier differs from the fixture')
                    elif any(abs(pins[p])>.02 for p in ('fan1','fan2')) or abs(pins['fan0_en']-1)>.02:
                        raise RuntimeError('Fixture cooling conditions differ from the prepared profiles')
                    first_stage=c.start(event)
                    activated=True
                    post(args.url,'/printer/gcode/script',{'script':command(first_stage)},key)
                    waiting_target_since=time.monotonic();time.sleep(.5);continue
                write_progress(args.progress_json,'running',stage=c.index+1,total=len(c.stages),
                               temperature=float(hot['temperature']),target=c.stage.target,flow=c.stage.flow)
                fan_changed=any(abs(cooling[name]-value)>.02 for name,value in cooling_baseline.items()) if protocol else any(abs(pins[p]-value)>.02 for p,value in (('fan0',1),('fan0_en',1),('fan1',0),('fan2',0)))
                if fan_changed:
                    raise RuntimeError('Fan conditions changed during calibration')
                if abs(float(s['gcode_move']['extrude_factor'])-1)>.001:
                    raise RuntimeError('Runtime extrusion multiplier differs from the fixture')
                if abs(float(hot['target'])-c.stage.target)>.2 and waiting_target_since is not None:
                    if time.monotonic()-waiting_target_since>3:raise RuntimeError('Temperature command was not applied')
                    time.sleep(.5);continue
                waiting_target_since=None
                next_stage=c.observe(event,float(hot['temperature']),float(hot['target']),flow)
                if next_stage=='complete':
                    cleanup_attempted=True
                    write_progress(args.progress_json,'stopping')
                    cleanup_status=cancel()
                    break
                if next_stage is not None:
                    post(args.url,'/printer/gcode/script',{'script':command(next_stage)},key)
                    waiting_target_since=time.monotonic()
                    print('Stage:',next_stage.name,flush=True)
                time.sleep(.5)
            else:raise TimeoutError('Total calibration time limit exceeded')
        except BaseException as error:
            write_progress(args.progress_json,'stopping')
            if cleanup_attempted:cleanup_error=str(error)
            if job_verified and not cleanup_attempted:
                cleanup_attempted=True
                try:cleanup_status=cancel()
                except Exception as error:
                    cleanup_error=str(error)
                    print('Could not confirm cancellation; cancel the calibration in the printer UI.',flush=True)
            raise
        finally:
            with open(args.output+'.summary.json','x',encoding='utf-8') as summary:
                json.dump({'completed':c.done,'stages':c.results,'flow_source':'commanded extruder velocity; not a melt measurement',
                           'printing_load_test':True,'sensor_response_only':True,'cancellation_confirmed':cleanup_status is not None,
                           'cleanup_error':cleanup_error,'generated_protocol':protocol,'cooling_baseline':cooling_baseline},summary,indent=2)
            state='completed' if c.done and cleanup_status is not None else 'stopped' if isinstance(sys.exc_info()[1],CalibrationStopped) and cleanup_status is not None else 'error'
            write_progress(args.progress_json,state,cleanup_confirmed=cleanup_status is not None,measurement_complete=c.done)

if __name__=='__main__':sys.exit(main())
