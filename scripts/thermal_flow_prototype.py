"""Offline flow-envelope prototype. No G-code, printer I/O or profile changes.

The path coordinate is extruded volume. Constant dQ/dt makes Q^2 linear
in volume: d(Q^2)/dV = 2*dQ/dt. Forward/backward envelopes can therefore
respect local flow ceilings while anticipating a later slowdown. Layer
boundaries do not reset the envelope. This is a slew-limited planning
reference, not an identified thermal or gloss prediction.
"""
from dataclasses import dataclass
import math


@dataclass(frozen=True)
class FlowRegion:
    volume_mm3: float
    maximum_flow_mm3_s: float
    label: str = ''


@dataclass(frozen=True)
class FlowNode:
    volume_mm3: float
    time_s: float
    flow_mm3_s: float
    required_temperature_c: float
    label: str


@dataclass(frozen=True)
class ThermalFlowPlan:
    nodes: tuple
    baseline_duration_s: float
    rise_limit_mm3_s2: float
    fall_limit_mm3_s2: float
    delay_s: float

    @property
    def duration_s(self):
        return self.nodes[-1].time_s

    def advanced_target(self, time_s):
        """Desired temperature led by a pure-delay approximation only."""
        if not math.isfinite(time_s) or time_s < 0:
            raise ValueError('Invalid planning time')
        t=min(time_s+self.delay_s,self.duration_s)
        for a,b in zip(self.nodes,self.nodes[1:]):
            if t<=b.time_s:
                f=(t-a.time_s)/(b.time_s-a.time_s)
                # Temperature table can cross a slope boundary. Interpolating
                # node temperatures is a display approximation, not G-code.
                return a.required_temperature_c+f*(b.required_temperature_c-a.required_temperature_c)
        return self.nodes[-1].required_temperature_c


def plan_thermal_flow(regions, table, initial_flow, rise_s_per_c, fall_s_per_c,
                      smoothing_period_s=5, delay_s=0, volume_step_mm3=.25):
    """Greatest discrete flow minorant with bounded temporal rise and fall.

    Local requested flow is a ceiling; acceleration, travel, retraction,
    pressure advance, macro time and native export are outside this prototype.
    An incompatible initial state is rejected, never silently cooled/reset.
    """
    scalars=(initial_flow,rise_s_per_c,fall_s_per_c,smoothing_period_s,volume_step_mm3)
    if not all(math.isfinite(v) and v>0 for v in scalars) or not math.isfinite(delay_s) or delay_s<0:
        raise ValueError('Planning parameters must be finite and positive')
    if len(table)<2 or not regions:
        raise ValueError('A monotone table and a continuous extrusion path are required')
    if not all(math.isfinite(q) and math.isfinite(t) and q>0 for q,t in table):
        raise ValueError('Invalid table')
    slopes=[]
    for a,b in zip(table,table[1:]):
        if b[0]<=a[0] or b[1]<=a[1]:
            raise ValueError('Prototype requires strictly increasing flow and temperature')
        slopes.append((b[1]-a[1])/(b[0]-a[0]))
    low,high=table[0][0],table[-1][0]
    if not low<=initial_flow<=high:
        raise ValueError('Initial flow is outside the measured table')
    up=min((high-low)/smoothing_period_s,1/(max(slopes)*rise_s_per_c))
    down=min((high-low)/smoothing_period_s,1/(max(slopes)*fall_s_per_c))
    volumes=[0.0];caps=[];labels=[];baseline=0.0
    for region in regions:
        v,q=region.volume_mm3,region.maximum_flow_mm3_s
        if not math.isfinite(v) or v<=0 or not math.isfinite(q) or not low<=q<=high:
            raise ValueError('Each continuous region needs positive volume and a flow ceiling inside the table')
        count=math.ceil(v/volume_step_mm3)
        if len(volumes)+count>1000000:
            raise ValueError('Prototype path is too large; use a coarser volume step')
        if not caps:caps.append(q)
        else:caps[-1]=min(caps[-1],q)
        for _ in range(count):
            volumes.append(volumes[-1]+v/count);caps.append(q);labels.append(region.label)
        baseline+=v/q
    if caps[0]<initial_flow:
        raise ValueError('Initial thermal/flow state exceeds the first local ceiling; preconditioning required')
    q2=[q*q for q in caps];q2[0]=initial_flow**2
    for i in range(1,len(q2)):
        q2[i]=min(q2[i],q2[i-1]+2*up*(volumes[i]-volumes[i-1]))
    for i in range(len(q2)-2,-1,-1):
        q2[i]=min(q2[i],q2[i+1]+2*down*(volumes[i+1]-volumes[i]))
    if q2[0]<initial_flow**2-1e-8:
        raise ValueError('Insufficient preceding extrusion to cool before a local slowdown; preconditioning required')
    def temperature(q):
        for a,b in zip(table,table[1:]):
            if q<=b[0]+1e-9:
                return a[1]+(q-a[0])*(b[1]-a[1])/(b[0]-a[0])
        return table[-1][1]
    nodes=[];time_s=0
    for i,volume in enumerate(volumes):
        q=math.sqrt(q2[i])
        if i:time_s+=2*(volume-volumes[i-1])/(q+nodes[-1].flow_mm3_s)
        nodes.append(FlowNode(volume,time_s,q,temperature(q),labels[max(0,i-1)]))
    return ThermalFlowPlan(tuple(nodes),baseline,up,down,delay_s)
