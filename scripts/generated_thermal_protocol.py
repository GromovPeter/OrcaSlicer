"""Protocol carried by a natively sliced calibration file; no printer I/O."""
import hashlib
import json
import math
from pathlib import Path
from loaded_thermal_calibration import Stage

MARKER = b'; _ORCA_LOADED_THERMAL_V1 '
MAX_FILE_BYTES = 64 * 1024 * 1024

def load_fixture(path):
    source = Path(path)
    if not 0 < source.stat().st_size <= MAX_FILE_BYTES:
        raise ValueError('Calibration file size is outside the supported range')
    data = source.read_bytes()
    first = data.split(b'\n', 1)[0]
    if not first.startswith(MARKER):
        raise ValueError('Choose an Orca-generated loaded thermal calibration file')
    protocol = json.loads(first[len(MARKER):])
    validate_protocol(protocol)
    protocol['sha256'] = hashlib.sha256(data).hexdigest()
    protocol['bytes'] = len(data)
    return protocol

def validate_protocol(p):
    if p.get('kind') != 'orca_loaded_thermal' or p.get('version') != 1:
        raise ValueError('Unsupported calibration protocol')
    bounds = {'filament_diameter': (1, 3), 'nominal_flow': (0.05, 200),
              'fan': (0, 1), 'measurement_z': (0.01, 2000), 'height': (0.01, 2000),
              'stage_timeout_s': (30, 300), 'total_timeout_s': (60, 3600),
              'temperature_min': (1, 1000), 'temperature_max': (1, 1000)}
    for name, (low, high) in bounds.items():
        v = p.get(name)
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or not low <= v <= high:
            raise ValueError('Invalid calibration protocol field: ' + name)
    for name, low, high in (('warmup_layers', 2, 10000), ('total_layers', 3, 100000)):
        if type(p.get(name)) is not int or not low <= p[name] <= high:
            raise ValueError('Invalid layer count: ' + name)
    if p['total_layers'] <= p['warmup_layers'] or p['height'] <= p['measurement_z']:
        raise ValueError('Calibration has no measurement region')
    stages = p.get('stages')
    if not isinstance(stages, list) or len(stages) != 9:
        raise ValueError('Expected nine calibration stages')
    names = set()
    for s in stages:
        if not isinstance(s, dict) or not isinstance(s.get('name'), str) or not s['name'] or s['name'] in names:
            raise ValueError('Invalid or repeated stage name')
        names.add(s['name'])
        for field in ('flow', 'target'):
            if isinstance(s.get(field), bool) or not isinstance(s.get(field), (float, int)) or not math.isfinite(s[field]):
                raise ValueError('Invalid stage ' + field)
        if not 0.05 <= s['flow'] <= 200 or not p['temperature_min'] <= s['target'] <= p['temperature_max']:
            raise ValueError('Stage exceeds calibration limits')
        if not 1 <= 100*s['flow']/p['nominal_flow'] <= 1000:
            raise ValueError('Calibration speed scaling is outside supported limits')
    q0, q1, q2 = stages[0]['flow'], stages[4]['flow'], stages[7]['flow']
    t0, t1, t2 = stages[1]['target'], stages[0]['target'], stages[3]['target']
    expected = [(q0,t1),(q0,t0),(q0,t1),(q0,t2),(q1,t2),(q1,t1),(q1,t2),(q2,t2),(q1,t2)]
    if not q0 < q1 < q2 or t1-t0+1e-6 < 5 or t2-t1+1e-6 < 5 or q1 != p['nominal_flow']:
        raise ValueError('Invalid calibration flow/temperature ordering')
    if [(s['flow'],s['target']) for s in stages] != expected:
        raise ValueError('Invalid fixed-load response sequence')

def stages(p):
    return tuple(Stage(s['name'], s['flow'], s['target']) for s in p['stages'])

def stage_commands(stage, p):
    if stage not in stages(p):
        raise ValueError('Stage does not belong to the generated protocol')
    return f'M104 S{stage.target:g}\nM220 S{100*stage.flow/p["nominal_flow"]:g}'

def cooling_objects(names):
    result = {}
    for name in names:
        if name == 'fan' or name.startswith('fan_generic '): result[name] = 'speed'
        elif name.startswith('output_pin fan'): result[name] = 'value'
    part = 'fan' if 'fan' in result else 'output_pin fan0' if 'output_pin fan0' in result else None
    if part is None: raise RuntimeError('Part cooling telemetry is unavailable')
    return result, part

def fan_values(status, objects):
    values = {}
    for name, field in objects.items():
        v = status.get(name, {}).get(field)
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or not 0 <= v <= 1:
            raise RuntimeError('Cooling telemetry unavailable: ' + name)
        values[name] = float(v)
    return values

def first_layers_ready(status, flow, p):
    current = status.get('print_stats', {}).get('info') or {}
    position = status.get('motion_report', {}).get('live_position') or []
    return (current.get('current_layer', 0) >= p['warmup_layers']+1 and
            status.get('virtual_sdcard', {}).get('is_active') is True and len(position) >= 3 and
            p['measurement_z'] <= position[2] <= p['height'] and flow > 0.05)
