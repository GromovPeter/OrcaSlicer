from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).parents[1]))
from thermal_flow_prototype import FlowRegion, plan_thermal_flow

TABLE=((1,196),(10,213),(22,228))

class ThermalFlowTests(unittest.TestCase):
    def plan(self,regions,initial=1,**kwargs):
        return plan_thermal_flow(regions,TABLE,initial,2.4,2.2,**kwargs)

    def test_cooling_begins_before_a_short_slow_feature_and_crosses_layers(self):
        regions=[FlowRegion(300,22,'layer 1'),FlowRegion(300,22,'layer 2'),FlowRegion(2,1,'overhang'),FlowRegion(300,22,'layer 3')]
        plan=self.plan(regions)
        self.assertGreater(plan.duration_s,plan.baseline_duration_s)
        self.assertAlmostEqual(plan.nodes[-1].volume_mm3,902)
        near=[n for n in plan.nodes if 598<=n.volume_mm3<=602]
        self.assertTrue(all(n.flow_mm3_s<2 for n in near))
        before=[n for n in plan.nodes if 550<n.volume_mm3<590]
        self.assertGreater(before[0].flow_mm3_s,before[-1].flow_mm3_s)
        for a,b in zip(plan.nodes,plan.nodes[1:]):
            dt=b.time_s-a.time_s;dq=b.flow_mm3_s-a.flow_mm3_s
            self.assertLessEqual(dq/dt,plan.rise_limit_mm3_s2+1e-8)
            self.assertGreaterEqual(dq/dt,-plan.fall_limit_mm3_s2-1e-8)
            self.assertLessEqual(abs(b.required_temperature_c-a.required_temperature_c)/dt,1/2.2+1e-8)
            self.assertGreaterEqual(a.flow_mm3_s,1-1e-8)
            if a.volume_mm3>=600 and b.volume_mm3<=602:
                self.assertLessEqual(max(a.flow_mm3_s,b.flow_mm3_s),1+1e-8)

    def test_stable_flow_preserves_volume_and_time(self):
        plan=self.plan([FlowRegion(100,10)],initial=10)
        self.assertAlmostEqual(plan.duration_s,10)
        self.assertTrue(all(abs(n.flow_mm3_s-10)<1e-8 for n in plan.nodes))

    def test_short_path_cannot_hide_an_incompatible_initial_hot_state(self):
        with self.assertRaisesRegex(ValueError,'preconditioning'):
            self.plan([FlowRegion(1,22),FlowRegion(1,1)],initial=22)

    def test_no_flow_ceiling_is_raised(self):
        plan=self.plan([FlowRegion(100,10),FlowRegion(100,3),FlowRegion(100,10)])
        for node in plan.nodes:
            self.assertLessEqual(node.flow_mm3_s,3+1e-8 if 100<=node.volume_mm3<=200 else 10+1e-8)

    def test_flow_smoothing_can_be_stricter_than_thermal_slew(self):
        plan=self.plan([FlowRegion(100,22)],smoothing_period_s=1000)
        self.assertAlmostEqual(plan.rise_limit_mm3_s2,21/1000)

    def test_target_lead_is_separate_from_requested_flow(self):
        regions=[FlowRegion(100,22)]
        plan=self.plan(regions,delay_s=3.6)
        self.assertGreater(plan.advanced_target(0),plan.nodes[0].required_temperature_c)
        self.assertAlmostEqual(plan.advanced_target(plan.duration_s),plan.nodes[-1].required_temperature_c)

    def test_unsupported_travel_and_nonmonotone_tables_fail_explicitly(self):
        for q in (0,23,float('nan')):
            with self.assertRaises(ValueError):self.plan([FlowRegion(10,q)])
        with self.assertRaises(ValueError):
            plan_thermal_flow([FlowRegion(10,10)],((1,220),(10,210)),1,2,2)

if __name__=='__main__':unittest.main()
