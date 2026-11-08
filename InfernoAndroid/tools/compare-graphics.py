#!/usr/bin/env python3
"""Compare three matching 60s CPU/GLES reports, extracted from diagnostics ZIP.
No Apple inputs or dependencies. Output is evidence, never a claimed phone test.
"""
import argparse
import json
import statistics
from pathlib import Path


def compare(reports):
    groups = {"CPU": [], "GLES3": []}
    profile = None
    fields = ("device", "cores", "memory_gb", "width", "height", "tb_mb", "split_wx")
    for report in reports:
        if report.get("kind") != "ui-benchmark-v1" or not report.get("valid"):
            raise ValueError("Incomplete/invalid benchmark; record another full 60 seconds")
        mode = report["requested"]
        if mode not in groups or report["selected"] != mode:
            raise ValueError("Safety fallback is not a GLES benchmark")
        d = report["delta"]
        if d["actual_backend"] != (mode == "GLES3") or d["fallbacks"] or d["surface_changes"]:
            raise ValueError("Backend or surface changed during benchmark")
        if report["duration_s"] < 60 or d["engine_metrics_version"] != 1:
            raise ValueError("Need >=60 seconds and cumulative engine ABI v1")
        if max(report.get("thermal_status_start", 0), report.get("thermal_status_end", 0)) >= 3:
            raise ValueError("Severe thermal throttling; cool phone and repeat")
        current = tuple(report[f] for f in fields)
        if profile is not None and profile != current:
            raise ValueError("Profiles differ (resolution/config/device). Compare separately")
        profile = current
        groups[mode].append(report)
    if any(len(items) != 3 for items in groups.values()):
        raise ValueError("Exactly three CPU and three GLES3 reports are required")
    result = {"profile": dict(zip(fields, profile)), "runs_per_backend": 3}
    for mode, items in groups.items():
        per_frame = [r["delta"]["present_cpu_ns"] / r["delta"]["host_frames"] for r in items]
        pixman = [r["delta"]["pixman_cpu_ns"] / max(1, r["delta"]["main_cpu_ns"]) for r in items]
        p50 = [r["guest_p50_upper_ms"] for r in items]
        result[mode] = {
            "present_cpu_ms_per_host_frame_median": statistics.median(per_frame)/1e6,
            "present_cpu_ms_per_second_median": statistics.median(
                r["delta"]["present_cpu_ns"] / r["duration_s"] / 1e6 for r in items),
            "guest_p50_upper_ms_median": None if None in p50 else statistics.median(p50),
            "pixman_fraction_of_main_cpu_median": statistics.median(pixman),
        }
    cpu, gpu = result["CPU"], result["GLES3"]
    old = cpu["present_cpu_ms_per_host_frame_median"]
    reduction = 1-gpu["present_cpu_ms_per_host_frame_median"]/old if old else None
    result["present_cpu_reduction_fraction"] = reduction
    cp, gp = cpu["guest_p50_upper_ms_median"], gpu["guest_p50_upper_ms_median"]
    # Histogram gives intervals [upper-1, upper). Certify only when even the
    # worst compatible ratio is <=1.10; otherwise report uncertainty/regression.
    frame_ok = cp is not None and gp is not None and cp > 1 and gp <= (cp-1)*1.10
    result["guest_median_within_10pct_certified"] = frame_ok
    result["promotion_criterion_met"] = reduction is not None and reduction >= .20 and frame_ok
    result["investigate_2d_offload"] = gpu["pixman_fraction_of_main_cpu_median"] >= .15
    result["limits"] = "Same manual UI sequence required; CPU submission times are not GPU execution times. Histogram resolution 1ms."
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(compare([json.loads(p.read_text()) for p in args.reports]), indent=2))
    except (ValueError, KeyError, ZeroDivisionError) as e:
        parser.error(str(e))
