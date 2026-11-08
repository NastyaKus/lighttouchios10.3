#!/usr/bin/env python3
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("compare_graphics", Path(__file__).parents[1]/"tools/compare-graphics.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def reports():
    out=[]
    for mode in ("CPU","GLES3"):
        for _ in range(3):
            out.append(dict(kind="ui-benchmark-v1", valid=True, requested=mode, selected=mode,
                duration_s=60, device="synthetic", cores=4, memory_gb=4, width=828, height=1792,
                tb_mb=512, split_wx=False, guest_p50_upper_ms=17, thermal_status_start=0, thermal_status_end=0,
                delta=dict(actual_backend=int(mode=="GLES3"), fallbacks=0, surface_changes=0,
                    engine_metrics_version=1, present_cpu_ns=100000000 if mode=="CPU" else 70000000,
                    host_frames=600, pixman_cpu_ns=200, main_cpu_ns=1000)))
    return out

class GraphicsReportTest(unittest.TestCase):
    def test_criterion_uses_three_runs_and_pixman_gate(self):
        result=module.compare(reports())
        self.assertTrue(result["promotion_criterion_met"])
        self.assertTrue(result["investigate_2d_offload"])
        self.assertAlmostEqual(result["present_cpu_reduction_fraction"],.30)
    def test_incomplete_profiles_and_fallback_are_rejected(self):
        for key,value in [("valid",False),("width",414),("selected","CPU"),("thermal_status_end",3)]:
            data=reports(); data[-1][key]=value
            with self.assertRaises(ValueError):module.compare(data)
        with self.assertRaises(ValueError):module.compare(reports()[:-1])
    def test_guest_regression_or_overflow_prevents_promotion(self):
        for value in [22,None]:
            data=reports()
            for r in data[3:]:r["guest_p50_upper_ms"]=value
            self.assertFalse(module.compare(data)["promotion_criterion_met"])
    def test_more_fps_without_less_cpu_per_frame_is_not_a_win(self):
        data=reports()
        for r in data[3:]:r["delta"]["present_cpu_ns"]=100000000
        self.assertFalse(module.compare(data)["promotion_criterion_met"])

if __name__ == "__main__":unittest.main()
