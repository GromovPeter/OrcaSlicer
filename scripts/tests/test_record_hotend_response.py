import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('thermal_recorder', Path(__file__).parents[1] / 'record_hotend_response.py')
recorder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recorder)


class ThermalRecorderTests(unittest.TestCase):
    def response(self, fan=True):
        status = {'extruder': {'target': 230, 'temperature': 228.5, 'power': 0.7}}
        if fan:
            status['fan'] = {'speed': 0.5}
        return {'result': {'eventtime': 1234.5, 'status': status}}

    def test_query_uses_only_get_and_keeps_printer_monotonic_time(self):
        with patch.object(recorder, 'urlopen', return_value=io.BytesIO(json.dumps(self.response()).encode())) as request:
            sample = recorder.read_sample('http://localhost:7125', api_key='test-only-key')
        self.assertEqual(sample, [1234.5, 230, 228.5, 0.7, 0.5])
        sent = request.call_args.args[0]
        self.assertEqual(sent.get_method(), 'GET')
        self.assertEqual(sent.data, None)
        self.assertIn('/printer/objects/query?', sent.full_url)
        self.assertNotIn('/gcode/', sent.full_url)
        self.assertEqual(sent.headers['X-api-key'], 'test-only-key')

    def test_unknown_fan_is_missing_instead_of_assumed_off(self):
        with patch.object(recorder, 'urlopen', return_value=io.BytesIO(json.dumps(self.response(False)).encode())):
            self.assertEqual(recorder.read_sample('http://localhost')[4], '')

    def test_output_pin_fan_reads_its_normalized_value(self):
        document = self.response(False)
        document['result']['status']['output_pin fan0'] = {'value': 0.75}
        with patch.object(recorder, 'urlopen', return_value=io.BytesIO(json.dumps(document).encode())) as request:
            sample = recorder.read_sample('http://localhost', fan='output_pin fan0', fan_field='value')
        self.assertEqual(sample[4], 0.75)
        self.assertIn('output_pin+fan0=value', request.call_args.args[0].full_url)

    def test_extra_pin_conditions_are_recorded_and_checked(self):
        document = self.response()
        document['result']['status']['output_pin fan2'] = {'value': 0.0}
        with patch.object(recorder, 'urlopen', return_value=io.BytesIO(json.dumps(document).encode())):
            self.assertEqual(recorder.read_sample('http://localhost', required_pins={'fan2': 0})[-1], 0.0)
        document['result']['status']['output_pin fan2']['value'] = 0.5
        with patch.object(recorder, 'urlopen', return_value=io.BytesIO(json.dumps(document).encode())):
            with self.assertRaises(RuntimeError):
                recorder.read_sample('http://localhost', required_pins={'fan2': 0})

    def test_missing_required_pin_stops_recording(self):
        with patch.object(recorder, 'urlopen', return_value=io.BytesIO(json.dumps(self.response()).encode())):
            with self.assertRaises(RuntimeError):
                recorder.read_sample('http://localhost', required_pins={'fan2': 0})

    def test_api_failure_is_not_converted_to_a_temperature(self):
        with patch.object(recorder, 'urlopen', return_value=io.BytesIO(b'{"error":{"message":"not ready"}}')):
            with self.assertRaises(RuntimeError):
                recorder.read_sample('http://localhost')


if __name__ == '__main__':
    unittest.main()
