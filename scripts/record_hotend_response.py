#!/usr/bin/env python3
"""Read-only Moonraker temperature recorder; never sends G-code or changes a target.
API: https://moonraker.readthedocs.io/en/latest/external_api/printer/
"""
import argparse
import csv
import json
import math
import os
import time
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen


def read_json(base_url, path, api_key=None):
    headers = {'Accept': 'application/json'}
    if api_key:
        headers['X-Api-Key'] = api_key
    with urlopen(Request(base_url.rstrip('/') + path, headers=headers, method='GET'), timeout=3) as reply:
        document = json.load(reply)
    if 'error' in document:
        raise RuntimeError('Moonraker returned an API error')
    return document['result']


def read_sample(base_url, heater='extruder', fan='fan', api_key=None, fan_field='speed', required_pins=None):
    required_pins = required_pins or {}
    objects = {heater: 'temperature,target,power', fan: fan_field}
    objects.update({'output_pin ' + name: 'value' for name in required_pins})
    query = urlencode(objects)
    result = read_json(base_url, '/printer/objects/query?' + query, api_key)
    status = result['status']
    heat = status[heater]
    values = [float(result['eventtime']), float(heat['target']), float(heat['temperature'])]
    if not all(math.isfinite(value) for value in values):
        raise ValueError('Moonraker returned a nonfinite timestamp or temperature')
    pin_values = []
    for name, expected in required_pins.items():
        observed = status.get('output_pin ' + name, {}).get('value')
        if observed is None or not math.isfinite(float(observed)) or abs(float(observed) - expected) > 0.02:
            raise RuntimeError('Required output pin changed or is unavailable: ' + name)
        pin_values.append(float(observed))
    return values + [heat.get('power', ''), status.get(fan, {}).get(fan_field, '')] + pin_values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True, help='Moonraker base URL, including port when needed')
    parser.add_argument('--output', help='New output CSV; existing files are never replaced')
    parser.add_argument('--duration', type=float, default=600)
    parser.add_argument('--interval', type=float, default=1)
    parser.add_argument('--heater', default='extruder')
    parser.add_argument('--fan', default='fan', help='Part-cooling fan object, not the hotend heatsink fan')
    parser.add_argument('--fan-field', choices=('speed', 'value'), default='speed', help='Use value for an output_pin fan')
    parser.add_argument('--require-pin', action='append', default=[], metavar='NAME=VALUE', help='Record a normalized output pin and stop if it differs by more than 0.02')
    parser.add_argument('--constant-flow', type=float, help='Known constant deposited flow in mm3/s; omit if unknown')
    parser.add_argument('--probe', action='store_true', help='Only list available objects and read one sample')
    args = parser.parse_args()
    parsed = urlsplit(args.url)
    if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        parser.error('Use an HTTP(S) base URL without credentials or a query string')
    for name in ('duration', 'interval'):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) <= 0:
            parser.error(name + ' must be finite and positive')
    if args.interval < 0.25:
        parser.error('Use a sampling interval of at least 0.25 s')
    if args.constant_flow is not None and (not math.isfinite(args.constant_flow) or args.constant_flow < 0):
        parser.error('Constant flow must be finite and nonnegative')
    if not args.probe and not args.output:
        parser.error('--output is required for recording')
    pins = {}
    for requirement in args.require_pin:
        try:
            name, text = requirement.split('=', 1)
            expected = float(text)
            if not name or not math.isfinite(expected) or not 0 <= expected <= 1 or name in pins:
                raise ValueError()
            pins[name] = expected
        except ValueError:
            parser.error('--require-pin needs a unique name and normalized value from 0 to 1')
    key = os.environ.get('MOONRAKER_API_KEY')
    if args.probe:
        objects = read_json(args.url, '/printer/objects/list', key)['objects']
        print(json.dumps({'heater_present': args.heater in objects, 'fan_present': args.fan in objects,
                          'fan_candidates': [name for name in objects if name == 'fan' or name.startswith(('fan_generic ', 'output_pin '))],
                          'sample': read_sample(args.url, args.heater, args.fan, key, args.fan_field, pins)}, indent=2))
        return
    with open(args.output, 'x', newline='', encoding='utf-8') as output:
        output.write('# Recorded Moonraker sensor data; flow is a declared constant, not a measured extrusion rate.\n')
        output.write('# Required constant output pins: ' + json.dumps(pins, sort_keys=True) + '\n')
        writer = csv.writer(output)
        writer.writerow(['time_s', 'target_c', 'temperature_c', 'power', 'fan'] + ['pin_' + name for name in pins] + ['flow_mm3_s'])
        started = time.monotonic()
        first_event = previous = None
        count = 0
        try:
            while time.monotonic() - started < args.duration:
                tick = time.monotonic()
                values = read_sample(args.url, args.heater, args.fan, key, args.fan_field, pins)
                event = values[0]
                if previous is not None and event < previous:
                    raise RuntimeError('Printer clock moved backwards; start a new recording after a restart')
                if first_event is None:
                    first_event = event
                if previous is None or event > previous:
                    writer.writerow([format(event - first_event, '.6f')] + values[1:] +
                                    [args.constant_flow if args.constant_flow is not None else ''])
                    output.flush()
                    count += 1
                previous = event
                time.sleep(max(0, args.interval - (time.monotonic() - tick)))
        except KeyboardInterrupt:
            pass
        print(f'Saved {count} samples. No printer commands were sent.')


if __name__ == '__main__':
    main()
