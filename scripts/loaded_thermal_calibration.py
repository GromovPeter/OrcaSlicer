"""Adaptive under-load thermal calibration logic. No printer I/O in this module."""
from collections import deque
from dataclasses import dataclass
import math


@dataclass(frozen=True)
class Stage:
    name: str
    flow: float
    target: float


PLA_STAGES = (
    Stage('baseline', 1, 213),
    Stage('cool_at_1', 1, 196),
    Stage('heat_at_1', 1, 213),
    Stage('heat_high_at_1', 1, 228),
    Stage('load_10_at_228', 10, 228),
    Stage('cool_at_10', 10, 213),
    Stage('heat_at_10', 10, 228),
    Stage('load_22_at_228', 22, 228),
    Stage('unload_to_10_at_228', 10, 228),
)


def fit_sensor_step(samples, old_target, new_target):
    delta=new_target-old_target
    if abs(delta)<5:return {}
    direction=1 if delta>0 else -1
    crossings=[]
    for fraction in (.1,.9):
        level=old_target+fraction*delta
        crossing=None
        for a,b in zip(samples,samples[1:]):
            if direction*(a[1]-level)<0<=direction*(b[1]-level):
                crossing=a[0]+(b[0]-a[0])*(level-a[1])/(b[1]-a[1]);break
        if crossing is None:return {}
        crossings.append(crossing)
    if crossings[1]<=crossings[0]:return {}
    rate=(crossings[1]-crossings[0])/(.8*abs(delta))
    return {'response_seconds_per_degree':rate,
            'extrapolated_delay_s':max(0,crossings[0]-samples[0][0]-.1*abs(delta)*rate)}


class LoadedCalibration:
    def __init__(self, hold_s=10, timeout_s=90, flow_window_s=5):
        self.hold_s, self.timeout_s, self.flow_window_s = hold_s, timeout_s, flow_window_s
        self.index = -1
        self.started = self.last_time = self.stable_since = None
        self.flow_samples = deque()
        self.results = []
        self.stage_samples = []
        self.load_disrupted = False
        self.done = False

    @property
    def stage(self):
        return PLA_STAGES[self.index] if self.index >= 0 and not self.done else None

    def start(self, time_s):
        if self.index != -1 or not math.isfinite(time_s):
            raise ValueError('Calibration already started or invalid time')
        self.index = 0
        self.started = time_s
        return self.stage

    def observe(self, time_s, actual_c, target_c, commanded_flow):
        if self.stage is None:
            raise RuntimeError('Calibration is not running')
        if not all(math.isfinite(v) for v in (time_s, actual_c, target_c, commanded_flow)):
            raise ValueError('Nonfinite measurement')
        if self.last_time is not None:
            if time_s <= self.last_time:
                raise ValueError('Printer time must increase')
            if time_s - self.last_time > 2.5:
                raise RuntimeError('Sampling gap; calibration excluded')
        self.last_time = time_s
        self.stage_samples.append((time_s,actual_c))
        if abs(target_c - self.stage.target) > 0.2:
            raise RuntimeError('Unexpected nozzle target; calibration stopped')
        if time_s - self.started >= self.timeout_s:
            raise TimeoutError('Target or extrusion load did not stabilize in time')
        if commanded_flow < -0.05:
            raise RuntimeError('Retraction detected in continuous-extrusion calibration')
        self.flow_samples.append((time_s, max(0.0, commanded_flow)))
        while len(self.flow_samples) > 1 and self.flow_samples[1][0] <= time_s - self.flow_window_s:
            self.flow_samples.popleft()
        span = time_s - self.flow_samples[0][0]
        mean_flow = None
        if span >= self.flow_window_s:
            integral = sum(0.5 * (a[1] + b[1]) * (b[0] - a[0])
                           for a, b in zip(self.flow_samples, list(self.flow_samples)[1:]))
            mean_flow = integral / span
        flow_tolerance=max(0.1,0.1*self.stage.flow)
        if commanded_flow<=0.05 or (mean_flow is not None and abs(mean_flow-self.stage.flow)>flow_tolerance):
            self.load_disrupted=True
        stable = mean_flow is not None and abs(mean_flow - self.stage.flow) <= flow_tolerance
        stable = stable and abs(actual_c - self.stage.target) <= 1.0
        # A stopped nozzle never satisfies a continuous-load dwell even while
        # the rolling mean still contains the preceding extrusion.
        stable = stable and commanded_flow > 0.05
        if not stable:
            self.stable_since = None
            return None
        if self.stable_since is None:
            self.stable_since = time_s
        if time_s - self.stable_since < self.hold_s:
            return None
        self.results.append({'stage': self.stage.name, 'flow_mm3_s': self.stage.flow,
                             'target_c': self.stage.target, 'elapsed_s': time_s - self.started,
                             'settled_at_s': time_s, 'rolling_commanded_flow_mm3_s': mean_flow,
                             'load_disrupted_during_stage':self.load_disrupted})
        if len(self.results)>1:
            previous=self.results[-2]
            if previous['flow_mm3_s']==self.stage.flow and not self.load_disrupted:
                self.results[-1].update(fit_sensor_step(self.stage_samples,previous['target_c'],self.stage.target))
            elif previous['flow_mm3_s']!=self.stage.flow:
                self.results[-1]['response_type']='load_change_at_constant_target'
            else:
                self.results[-1]['response_excluded']='extrusion load changed during target response'
        self.stage_samples=[(time_s,actual_c)]
        self.load_disrupted=False
        self.index += 1
        self.started = time_s
        self.stable_since = None
        self.flow_samples.clear()
        if self.index == len(PLA_STAGES):
            self.done = True
            return 'complete'
        return self.stage
