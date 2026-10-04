"""Public synthetic evidence; no game/server process or runtime dependency."""
import copy
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
try:
    import calibration
except ImportError:
    calibration = None


def manifest():
    return {"schema": "modperf-bench/manifest/1", "scenario_id": "synthetic",
            "config_sha256": "1" * 64, "world_sha256": "2" * 64,
            "engine_sha256": "3" * 64, "scenario_snapshot_sha256": "4" * 64,
            "host_id": "test-host", "platform": "test",
            "observer_mode": "recording", "fps_limit": None, "mods": [],
            "effective_config_comparison_sha256": "5" * 64, "effective_config_normalization": "mission-template-path/1"}


def window(values):
    count = len(values)
    mean = sum(values) / count
    return {"frames": count * 100, "blocks": count, "mean_ms": mean,
            "median_ms": sorted(values)[count // 2], "p95_ms": max(values),
            "median_fps": 1000 / mean, "elapsed_s": sum(values) / 10,
            "samples_ms": values, "sample_frames": [100] * count,
            "sample_elapsed_s": [v / 10 for v in values], "samples_kept": count,
            "sample_end_s": list(itertools.accumulate(v / 10 for v in values)),
            "decimated": False, "timing_source": "entry_wall_clock", "tail_coverage": False}


def result(values=None, tax=None, repetitions=3):
    values = values or [10.0] * 40
    config = {"note": "synthetic", "hardware_note": "test", "settle_seconds": 1,
              "window_seconds": 40, "block_seconds": .01, "a_prime_tolerance_pct": 10,
              "model_ns_timer_residency": 0, "b2_k": 500, "b5_units": 1000,
              "b3_lookups": 100000, "b3_repeats": 5, "b3_entity": "test",
              "b3_fallback_entity": "test", "b3_slot": "", "b3_attachment": "test",
              "b3_position": "0 0 0", "benches": ["BASELINE"], "k_values": [100],
              "b2_periods_ms": [0], "repeats": repetitions, "max_samples": 2000,
              "manifest_sha256": "", "scenario_id": "synthetic",
              "transition_seconds": 1, "run_self_tests": 1}
    benches = []
    for rep in range(repetitions):
        windows = {"baseline": window(values)}
        if tax is not None:
            windows.update(treatment=window([v + tax for v in values]), recovery=window(values))
        benches.append({"id": "B1" if tax is not None else "BASELINE", "variant": "test",
                        "protocol": "a_b_a_prime" if tax is not None else "observed_baseline",
                        "mechanism": "synthetic", "units": 100, "period_ms": 0,
                        "replicate": rep, "validity_reasons": [],
                        "verdict": "REQUIRES_ANALYSIS", "windows": windows})
    return {"schema": "modperf-bench/results/2", "protocol_version": "2.0", "manifest_sha256": "", "config": config,
            "clock": {"source": "GetTickTime", "frame_pairing_valid": False,
            "observer_qualified": False}, "benches": benches}


def inventory():
    return {"id": "B3", "protocol": "single_frame_balanced_pairs_v2", "status": "MEASURED",
            "verdict": "MEASURED", "ok": True, "validity_reasons": [], "lookups": 1000,
            "repeats": 2, "repeats_per_pair": 1, "pair_count": 2, "total_calls": 2000,
            "hits": 2000, "control_hits": 2000, "control_loop_ms": 4., "measured_loop_ms": 6.,
            "per_call_ns_net": 1000., "clock_step_ms": .001, "quantization_bound_ms": .004,
            "per_call_ns_resolution": 2., "resolution_bound_ns_lower": 998.,
            "resolution_bound_ns_upper": 1002., "resolution_bound_kind": "observed_clock_quantization_only",
            "pairs": [
                {"index": 0, "order": "control_lookup", "calls": 1000, "control_ms": 2., "lookup_ms": 3., "delta_ms": 1.},
                {"index": 1, "order": "lookup_control", "calls": 1000, "control_ms": 2., "lookup_ms": 3., "delta_ms": 1.}]}


class CalibrationTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(calibration, "offline calibration analyzer must exist")

    def test_v1_is_historical_even_with_measured_verdict(self):
        report = calibration.analyze({"schema": "modperf-bench/results/1", "benches": [{"verdict": "MEASURED"}]})
        self.assertEqual(report["status"], "historical_unqualified")
        self.assertIsNone(report["baseline"])
        self.assertEqual(report["capacity"]["status"], "not_established")

    def test_baseline_is_total_frames_divided_by_elapsed(self):
        data = result([10., 20.] * 20)
        report = calibration.analyze(data)
        self.assertAlmostEqual(report["baseline"]["mean_frame_ms"], 15.)
        self.assertAlmostEqual(report["baseline"]["observed_rate_fps"], 1000 / 15)
        self.assertEqual(report["status"], "observed_unqualified")
        self.assertFalse(report["baseline"]["individual_frame_tails_available"])

    def test_unknown_weights_prevent_uncertainty_not_become_zero(self):
        data = result(tax=2.)
        for bench in data["benches"]:
            for win in bench["windows"].values():
                del win["sample_frames"]
                del win["sample_elapsed_s"]
        estimate = calibration.analyze(data)["primitives"][0]
        self.assertIsNone(estimate["interval_ms"])
        self.assertIn("missing_block_weights", estimate["reasons"])

    def test_known_load_and_identical_arms_have_deterministic_intervals(self):
        loaded = calibration.analyze(result(tax=2.))
        self.assertEqual(loaded, calibration.analyze(result(tax=2.)))
        estimate = loaded["primitives"][0]
        self.assertAlmostEqual(estimate["delta_ms"], 2.)
        self.assertAlmostEqual(estimate["per_unit_ns"], 20000.)
        self.assertGreater(estimate["interval_ms"][0], 0)
        null = calibration.analyze(result(tax=0.))["primitives"][0]
        self.assertEqual(null["status"], "unresolved")
        self.assertLessEqual(null["interval_ms"][0], 0)
        self.assertGreaterEqual(null["interval_ms"][1], 0)

    def test_correlation_increases_block_length(self):
        rng = random.Random(91)
        values, state = [], 0.
        for _ in range(200):
            state = .94 * state + rng.gauss(0, .2)
            values.append(10 + state)
        estimate = calibration.analyze(result(values, tax=.5))["primitives"][0]
        self.assertGreater(estimate["bootstrap"]["block_length"], 1)
        self.assertGreater(estimate["interval_ms"][1], estimate["interval_ms"][0])

    def test_invalid_or_incomplete_capture_cannot_create_estimate(self):
        for mutation in ("nonfinite", "negative", "missing", "decimated", "invalid", "protocol", "mean"):
            with self.subTest(mutation=mutation):
                data = result(tax=2., repetitions=1)
                win = data["benches"][0]["windows"]["treatment"]
                if mutation == "nonfinite": win["samples_ms"][0] = math.nan
                elif mutation == "negative": win["elapsed_s"] = -1
                elif mutation == "missing": win["samples_ms"].pop()
                elif mutation == "decimated": win["decimated"] = True
                elif mutation == "invalid": data["benches"][0]["verdict"] = "INVALID_NO_FIRES"
                elif mutation == "protocol": data["protocol_version"] = "unknown"
                elif mutation == "mean": win["mean_ms"] = 1.
                report = calibration.analyze(data)
                self.assertEqual(report["status"], "invalid")
                self.assertIsNone(report["baseline"])
                self.assertEqual(report["primitives"], [])

    def test_failed_treatment_does_not_invalidate_independent_baseline(self):
        data = result()
        bad = result(tax=2., repetitions=1)["benches"][0]
        bad["verdict"], bad["windows"] = "INVALID_NO_FIRES", {}
        data["benches"].append(bad)
        report = calibration.analyze(data)
        self.assertEqual(report["status"], "observed_unqualified")
        self.assertEqual(report["baseline"]["mean_frame_ms"], 10.)
        self.assertEqual(len(report["rejected_benches"]), 1)

    def test_inventory_summary_cannot_supply_whole_frame_timing(self):
        data = result()
        data["benches"].append({"id": "B3", "protocol": "single_frame_loop",
                                "verdict": "MEASURED", "per_call_ns_net": 1.})
        report = calibration.analyze(data)
        self.assertEqual(report["status"], "observed_unqualified")
        self.assertEqual(report["baseline"]["mean_frame_ms"], 10.)
        self.assertEqual(report["independent_summaries"][0]["status"], "historical_summary_unqualified")

    def test_mismatched_stack_manifest_blocks_attribution(self):
        ref, target = manifest(), manifest()
        target["world_sha256"] = "4" * 64
        report = calibration.analyze(result(), target, result(), ref)
        self.assertEqual(report["stack_comparison"]["status"], "not_comparable")
        self.assertIsNone(report["stack_comparison"]["delta_ms"])

    def test_manifest_unknown_identity_not_equal_even_on_both_arms(self):
        ref, target = manifest(), manifest()
        ref["world_sha256"] = target["world_sha256"] = None
        report = calibration.analyze(result(), target, result(), ref)
        self.assertEqual(report["stack_comparison"]["status"], "not_comparable")

    def test_capped_stack_does_not_report_zero_tax_or_headroom(self):
        ref, target = manifest(), manifest()
        ref["fps_limit"] = target["fps_limit"] = 50
        report = calibration.analyze(result([20.] * 40), target, result([20.] * 40), ref)
        comparison = report["stack_comparison"]
        self.assertEqual(comparison["status"], "cap_limited")
        self.assertIsNone(comparison["delta_ms"])
        self.assertIsNone(comparison["fps_loss_fraction"])
        self.assertIsNone(comparison["headroom_ms"])

    def test_stack_tax_is_frame_duration_then_fps(self):
        report = calibration.analyze(result([12.] * 40), manifest(), result([10.] * 40), manifest())
        contrast = report["stack_comparison"]
        self.assertAlmostEqual(contrast["delta_ms"], 2.)
        self.assertAlmostEqual(contrast["fps_loss_fraction"], 1 / 6)
        self.assertEqual(contrast["status"], "estimated_unqualified")

    def test_bad_config_and_manifest_types_are_invalid(self):
        for key, value in (("a_prime_tolerance_pct", -1), ("benches", []),
                           ("k_values", [False]), ("note", None), ("repeats", True)):
            with self.subTest(key=key):
                data = result()
                data["config"][key] = value
                self.assertEqual(calibration.analyze(data)["status"], "invalid")
        mf = manifest()
        mf["fps_limit"] = "uncapped"
        self.assertEqual(calibration.analyze(result(), mf, result(), manifest())["stack_comparison"]["status"], "not_comparable")

    def test_snapshot_change_blocks_attribution(self):
        mf = manifest()
        mf["scenario_snapshot_sha256"] = "5" * 64
        comparison = calibration.analyze(result(), mf, result(), manifest())["stack_comparison"]
        self.assertEqual(comparison["status"], "not_comparable")

    def test_windows_reconstruct_exact_mean_with_unequal_block_weights(self):
        data = result()
        for bench in data["benches"]:
            w = window([10., 20.] * 20)
            w["sample_frames"] = [100, 200] * 20
            w["sample_elapsed_s"] = [1., 4.] * 20
            w["sample_end_s"] = list(itertools.accumulate(w["sample_elapsed_s"]))
            w["frames"], w["elapsed_s"], w["mean_ms"] = 6000, 100., 1000 / 60
            bench["windows"]["baseline"] = w
        report = calibration.analyze(data)
        self.assertAlmostEqual(report["baseline"]["mean_frame_ms"], 1000 / 60)
        self.assertAlmostEqual(report["baseline"]["observed_rate_fps"], 60.)

    def test_simulated_correlated_null_false_positive_budget(self):
        # Entire repetitions are independent, adjacent blocks are correlated.
        false_positives = 0
        for seed in range(10):
            data, rng = result(tax=0.), random.Random(seed + 200)
            for bench in data["benches"]:
                for name in bench["windows"]:
                    values, state = [], 0.
                    for _ in range(80):
                        state = .8 * state + rng.gauss(0, .1)
                        values.append(10 + state)
                    bench["windows"][name] = window(values)
            interval = calibration.analyze(data)["primitives"][0]["interval_ms"]
            false_positives += int(not interval[0] <= 0 <= interval[1])
        self.assertLessEqual(false_positives, 2)

    def test_json_truncation_duplicate_keys_and_nan_are_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            artifact = Path(tmp) / "bad.json"
            for text in ('{"schema":', '{"schema":"a","schema":"b"}', '{"v":NaN}'):
                artifact.write_text(text, encoding="utf-8")
                self.assertEqual(calibration.analyze_files(artifact)["status"], "invalid")

    def test_malformed_values_are_rejected_without_crashing(self):
        for field, value in (("mean_ms", "bad"), ("frames", 10 ** 1000), ("samples_ms", [True] * 40)):
            with self.subTest(field=field):
                data = result(repetitions=1)
                data["benches"][0]["windows"]["baseline"][field] = value
                self.assertEqual(calibration.analyze(data)["status"], "invalid")
        for field in ("variant", "id", "replicate", "period_ms"):
            with self.subTest(field=field):
                data = result(tax=1., repetitions=1)
                data["benches"][0][field] = []
                self.assertEqual(calibration.analyze(data)["status"], "invalid")

    def test_missing_clock_flags_remain_unqualified_with_reason(self):
        data = result()
        del data["clock"]
        report = calibration.analyze(data)
        self.assertEqual(report["status"], "observed_unqualified")
        self.assertIn("clock_qualification_evidence_missing", report["reasons"])

    def test_cli_checks_manifest_bytes_hash_and_writes_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            mf = folder / "manifest.json"
            mf.write_text(json.dumps(manifest()), encoding="utf-8")
            data = result()
            data["config"]["manifest_sha256"] = hashlib.sha256(mf.read_bytes()).hexdigest()
            data["manifest_sha256"] = data["config"]["manifest_sha256"]
            rf, output = folder / "results.json", folder / "analysis.json"
            rf.write_text(json.dumps(data), encoding="utf-8")
            args = [sys.executable, str(ROOT / "tools" / "analyze-results.py"), str(rf),
                    "--manifest", str(mf), "--output", str(output)]
            run = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(output.read_text())["status"], "observed_unqualified")
            mf.write_text(json.dumps(manifest()) + " ", encoding="utf-8")
            run = subprocess.run(args, capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(json.loads(output.read_text())["status"], "invalid")

    def test_cli_preserves_input_when_output_path_matches(self):
        with tempfile.TemporaryDirectory() as tmp:
            rf = Path(tmp) / "results.json"
            source = json.dumps(result())
            rf.write_text(source, encoding="utf-8")
            run = subprocess.run([sys.executable, str(ROOT / "tools" / "analyze-results.py"),
                                  str(rf), "--output", str(rf)], capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(rf.read_text(encoding="utf-8"), source)

    def test_inventory_failures_and_bad_pairs_are_rejected_independently(self):
        for mutation in ("SPAWN_FAILED", "BELOW_RESOLUTION", "NEGATIVE_DELTA", "ok", "gate", "order", "calls", "delta", "totals", "empty"):
            with self.subTest(mutation=mutation):
                data, bench = result(), inventory()
                if mutation in ("SPAWN_FAILED", "BELOW_RESOLUTION", "NEGATIVE_DELTA"):
                    bench.update(verdict=mutation, status=mutation, per_call_ns_net=None, ok=False)
                elif mutation == "ok": bench["ok"] = False
                elif mutation == "gate": bench["validity_reasons"] = ["bad_clock"]
                elif mutation == "order": bench["pairs"][1]["order"] = "control_lookup"
                elif mutation == "calls": bench["pairs"][0]["calls"] = 2000
                elif mutation == "delta": bench["pairs"][0]["delta_ms"] = -1.
                elif mutation == "totals": bench["control_loop_ms"] = 10.
                elif mutation == "empty": bench["pairs"] = []
                data["benches"].append(bench)
                report = calibration.analyze(data)
                self.assertEqual(report["status"], "observed_unqualified")
                self.assertEqual(len(report["rejected_benches"]), 1)
                self.assertEqual(report["independent_summaries"], [])

    def test_valid_balanced_inventory_pairs_remain_unqualified(self):
        data = result()
        data["benches"].append(inventory())
        report = calibration.analyze(data)
        self.assertEqual(report["rejected_benches"], [])
        self.assertEqual(report["independent_summaries"][0]["status"], "estimated_unqualified")

    def test_missing_ordered_timestamps_preserves_mean_but_blocks_uncertainty(self):
        data = result(tax=2.)
        for bench in data["benches"]:
            for w in bench["windows"].values(): del w["sample_end_s"]
        report = calibration.analyze(data)
        self.assertEqual(report["baseline"]["mean_frame_ms"], 10.)
        estimate = report["primitives"][0]
        self.assertIsNone(estimate["interval_ms"])
        self.assertIn("missing_ordered_timestamps", estimate["reasons"])

    def test_monotonic_but_noncontiguous_timestamps_are_rejected(self):
        for mutation in ("gap", "offset"):
            with self.subTest(mutation=mutation):
                data = result(repetitions=1)
                w = data["benches"][0]["windows"]["baseline"]
                w["sample_end_s"] = [v + (1 if mutation == "offset" or i > 15 else 0) for i, v in enumerate(w["sample_end_s"])]
                self.assertEqual(calibration.analyze(data)["status"], "invalid")

    def test_each_result_scenario_is_bound_to_its_manifest(self):
        target, source = result(), result()
        target["config"]["scenario_id"] = source["config"]["scenario_id"] = "other"
        report = calibration.analyze(target, manifest(), source, manifest())
        self.assertEqual(report["status"], "invalid")
        self.assertIn("scenario_manifest_mismatch", report["reasons"])
        source["config"]["scenario_id"] = "other"
        report = calibration.analyze(result(), manifest(), source, manifest())
        self.assertEqual(report["stack_comparison"]["status"], "not_comparable")

    def test_generated_server_config_path_hash_is_not_workload_identity(self):
        target, source = manifest(), manifest()
        target["effective_config_sha256"], source["effective_config_sha256"] = "a" * 64, "b" * 64
        contrast = calibration.analyze(result([12.] * 40), target, result(), source)["stack_comparison"]
        self.assertAlmostEqual(contrast["delta_ms"], 2.)

    def test_effective_server_setting_change_blocks_comparison(self):
        target = manifest()
        target["effective_config_comparison_sha256"] = "6" * 64
        comparison = calibration.analyze(result(), target, result(), manifest())["stack_comparison"]
        self.assertEqual(comparison["status"], "not_comparable")
        self.assertIn("mismatched_identity:effective_config_comparison_sha256", comparison["reasons"])

    def test_missing_effective_server_identity_blocks_comparison(self):
        target, source = manifest(), manifest()
        del target["effective_config_comparison_sha256"]
        del source["effective_config_comparison_sha256"]
        self.assertEqual(calibration.analyze(result(), target, result(), source)["stack_comparison"]["status"], "not_comparable")

    def test_null_ordered_timestamps_and_malformed_configs_stay_honest(self):
        data = result(tax=2.)
        for bench in data["benches"]:
            for w in bench["windows"].values(): w["sample_end_s"] = None
        estimate = calibration.analyze(data)["primitives"][0]
        self.assertIsNone(estimate["interval_ms"])
        for value in (None, [], "bad"):
            bad = result()
            bad["config"] = value
            self.assertEqual(calibration.analyze(bad, manifest())["status"], "invalid")

    def test_cli_requires_top_and_config_manifest_byte_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            mf, rf = Path(tmp) / "manifest.json", Path(tmp) / "results.json"
            mf.write_text(json.dumps(manifest()), encoding="utf-8")
            data = result()
            data["config"]["manifest_sha256"] = hashlib.sha256(mf.read_bytes()).hexdigest()
            rf.write_text(json.dumps(data), encoding="utf-8")
            self.assertEqual(calibration.analyze_files(rf, mf)["status"], "invalid")

    def test_v2_requires_both_manifest_hash_echoes_even_when_unknown(self):
        data = result()
        del data["manifest_sha256"]
        self.assertEqual(calibration.analyze(data)["status"], "invalid")

    def test_inventory_clock_and_resolution_gates_cannot_be_faked(self):
        mutations = [("clock_step_ms", None), ("clock_step_ms", 0.), ("clock_step_ms", -1.),
                     ("quantization_bound_ms", .001), ("per_call_ns_resolution", .1),
                     ("resolution_bound_ns_lower", 999.), ("resolution_bound_ns_upper", 1001.),
                     ("resolution_bound_kind", "confidence_interval")]
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                data, bench = result(), inventory()
                bench[field] = value
                data["benches"].append(bench)
                report = calibration.analyze(data)
                self.assertEqual(len(report["rejected_benches"]), 1)
                self.assertEqual(report["independent_summaries"], [])
        data, bench = result(), inventory()
        # Internally consistent positive values still do not resolve a cost
        # when their quantization bound is larger than the net effect.
        bench.update(clock_step_ms=1., quantization_bound_ms=4., per_call_ns_resolution=2000.,
                     resolution_bound_ns_lower=-1000., resolution_bound_ns_upper=3000.)
        data["benches"].append(bench)
        self.assertEqual(len(calibration.analyze(data)["rejected_benches"]), 1)


if __name__ == "__main__":
    unittest.main()
