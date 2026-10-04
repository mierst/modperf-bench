"""Offline, dependency-free analysis of bounded ordered benchmark evidence.

Block means describe elapsed entry gaps, not CPU service time or individual
frame tails. No artifact accepted here establishes operational population.
"""
from collections import defaultdict
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import statistics

ANALYZER_VERSION = "1.0"
ITERATIONS = 1000
SEED = 20261003
CONFIG_KEYS = {
    "note", "hardware_note", "settle_seconds", "window_seconds", "block_seconds",
    "a_prime_tolerance_pct", "model_ns_timer_residency", "b2_k", "b5_units",
    "b3_lookups", "b3_repeats", "b3_entity", "b3_fallback_entity", "b3_slot",
    "b3_attachment", "b3_position", "benches", "k_values", "b2_periods_ms",
    "repeats", "max_samples", "manifest_sha256", "scenario_id", "transition_seconds", "run_self_tests",
}
MATCH_KEYS = ("scenario_id", "config_sha256", "world_sha256", "engine_sha256",
              "scenario_snapshot_sha256", "host_id", "platform", "observer_mode", "fps_limit",
              "effective_config_comparison_sha256", "effective_config_normalization")


def _number(value, positive=False, integer=False):
    try:
        return (type(value) in (int, float) and math.isfinite(value)
                and (not positive or value > 0)
                and (not integer or type(value) is int))
    except OverflowError:
        return False


def _hash(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def _finite_tree(value):
    if type(value) is float:
        return math.isfinite(value)
    if isinstance(value, dict):
        return all(_finite_tree(v) for v in value.values())
    if isinstance(value, list):
        return all(_finite_tree(v) for v in value)
    return True


def validate(results):
    """Return errors; no failed or truncated evidence becomes a trusted number."""
    errors = []
    if not isinstance(results, dict):
        return ["results_not_object"]
    if results.get("schema") == "modperf-bench/results/1":
        return []
    if results.get("schema") != "modperf-bench/results/2":
        errors.append("unsupported_schema")
    if results.get("protocol_version") != "2.0":
        errors.append("unsupported_protocol")
    if not _finite_tree(results):
        errors.append("nonfinite_value")
    config = results.get("config")
    if not isinstance(config, dict) or not CONFIG_KEYS.issubset(config):
        errors.append("incomplete_effective_config")
    else:
        for key in ("settle_seconds", "window_seconds", "block_seconds", "transition_seconds"):
            if not _number(config[key], positive=True):
                errors.append("invalid_config:" + key)
        for key in ("b2_k", "b5_units", "b3_lookups", "b3_repeats", "repeats", "max_samples"):
            if not _number(config[key], positive=True, integer=True):
                errors.append("invalid_config:" + key)
        for key in ("a_prime_tolerance_pct", "model_ns_timer_residency"):
            if not _number(config[key]) or config[key] < 0:
                errors.append("invalid_config:" + key)
        for key in ("note", "hardware_note", "b3_entity", "b3_fallback_entity", "b3_slot", "b3_attachment", "b3_position", "scenario_id"):
            if not isinstance(config[key], str):
                errors.append("invalid_config:" + key)
        if type(config["run_self_tests"]) is not int or config["run_self_tests"] not in (0, 1):
            errors.append("invalid_config:run_self_tests")
        for key in ("k_values", "b2_periods_ms"):
            if not isinstance(config[key], list) or not config[key] or any(not _number(v, integer=True) or v < (1 if key == "k_values" else 0) for v in config[key]):
                errors.append("invalid_config:" + key)
        if not isinstance(config["benches"], list) or not config["benches"] or any(not isinstance(v, str) or not v for v in config["benches"]):
            errors.append("invalid_config:benches")
        if config["manifest_sha256"] not in ("", None) and not _hash(config["manifest_sha256"]):
            errors.append("invalid_manifest_hash")
        if "manifest_sha256" not in results or results["manifest_sha256"] != config["manifest_sha256"]:
            errors.append("manifest_hash_echo_mismatch")
    benches = results.get("benches")
    if not isinstance(benches, list) or not benches:
        return errors + ["missing_benches"]
    identities = set()
    for index, bench in enumerate(benches):
        prefix = f"bench[{index}]:"
        if not isinstance(bench, dict):
            errors.append(prefix + "not_object")
            continue
        if bench.get("id") == "B3" and bench.get("protocol") in ("single_frame_loop", "single_frame_balanced_pairs_v2", "paired_loops"):
            errors.extend(prefix + e for e in _validate_inventory(bench))
            continue
        if not isinstance(bench.get("id"), str) or not isinstance(bench.get("mechanism"), str):
            errors.append(prefix + "missing_identity")
            continue
        if not isinstance(bench.get("variant"), str):
            errors.append(prefix + "invalid_variant")
            continue
        if not _number(bench.get("replicate"), integer=True) or bench.get("replicate", -1) < 0:
            errors.append(prefix + "invalid_replicate")
            continue
        if not _number(bench.get("period_ms"), integer=True) or bench["period_ms"] < 0:
            errors.append(prefix + "invalid_period")
            continue
        identity = (bench.get("id"), bench.get("variant"), bench.get("replicate"))
        if identity in identities:
            errors.append(prefix + "duplicate_replicate")
        identities.add(identity)
        if not isinstance(bench.get("validity_reasons"), list) or bench.get("validity_reasons"):
            errors.append(prefix + "failed_validity_gate")
        if bench.get("verdict") not in ("REQUIRES_ANALYSIS", "OBSERVED"):
            errors.append(prefix + "failed_verdict")
        protocol = bench.get("protocol")
        if protocol not in ("observed_baseline", "a_b_a_prime"):
            errors.append(prefix + "unsupported_bench_protocol")
            continue
        windows = bench.get("windows")
        required = ("baseline",) if protocol == "observed_baseline" else ("baseline", "treatment", "recovery")
        if not isinstance(windows, dict) or any(k not in windows for k in required):
            errors.append(prefix + "missing_windows")
            continue
        if protocol == "a_b_a_prime" and not _number(bench.get("units"), positive=True, integer=True):
            errors.append(prefix + "invalid_units")
        for name in required:
            errors.extend(prefix + name + ":" + e for e in _validate_window(windows[name]))
    return errors


def _validate_inventory(bench):
    errors = []
    verdict = bench.get("verdict")
    if verdict != "MEASURED":
        return ["inventory_verdict:" + str(verdict)]
    if "status" in bench and bench["status"] != verdict:
        errors.append("inventory_status_mismatch")
    if "ok" in bench and bench["ok"] is not True:
        errors.append("inventory_ok_false")
    if "validity_reasons" in bench and bench["validity_reasons"] != []:
        errors.append("inventory_failed_validity_gate")
    if not _number(bench.get("per_call_ns_net"), positive=True):
        errors.append("invalid_inventory_summary")
    if bench["protocol"] == "single_frame_loop":
        return errors
    pairs = bench.get("pairs")
    if not isinstance(pairs, list) or not pairs:
        return errors + ["missing_inventory_pairs"]
    for key in ("lookups", "repeats", "repeats_per_pair", "pair_count", "total_calls"):
        if not _number(bench.get(key), positive=True, integer=True):
            errors.append("invalid_inventory_" + key)
    if errors:
        return errors
    if len(pairs) != bench["pair_count"] or len(pairs) % 2:
        errors.append("unbalanced_inventory_pair_count")
    if bench["total_calls"] != bench["lookups"] * bench["repeats"] or bench["repeats"] != len(pairs) * bench["repeats_per_pair"]:
        errors.append("inventory_amplification_mismatch")
    for i, pair in enumerate(pairs):
        if not isinstance(pair, dict):
            errors.append("invalid_inventory_pair")
            continue
        if type(pair.get("index")) is not int or pair["index"] != i or pair.get("order") != ("control_lookup" if i % 2 == 0 else "lookup_control"):
            errors.append("inventory_pair_order_mismatch")
        if type(pair.get("calls")) is not int or pair["calls"] != bench["lookups"] * bench["repeats_per_pair"]:
            errors.append("inventory_pair_calls_mismatch")
        if any(not _number(pair.get(k)) for k in ("control_ms", "lookup_ms", "delta_ms")):
            errors.append("invalid_inventory_pair_timing")
        elif pair["control_ms"] < 0 or pair["lookup_ms"] < 0 or not math.isclose(pair["delta_ms"], pair["lookup_ms"] - pair["control_ms"], rel_tol=.002, abs_tol=.0015):
            errors.append("inventory_pair_delta_mismatch")
    if errors:
        return errors
    for total, field in (("control_loop_ms", "control_ms"), ("measured_loop_ms", "lookup_ms")):
        if not _number(bench.get(total)) or not math.isclose(bench[total], sum(p[field] for p in pairs), rel_tol=.002, abs_tol=(len(pairs) + 1) * .0005):
            errors.append("inventory_loop_total_mismatch")
    if errors:
        return errors
    expected = (bench["measured_loop_ms"] - bench["control_loop_ms"]) * 1e6 / bench["total_calls"]
    if not math.isclose(bench["per_call_ns_net"], expected, rel_tol=.002, abs_tol=.5 + 1000 / bench["total_calls"]):
        errors.append("inventory_net_mismatch")
    for key in ("hits", "control_hits"):
        if not _number(bench.get(key), integer=True) or bench[key] not in (0, bench["total_calls"]):
            errors.append("inventory_hits_mismatch")
    if bench.get("hits") != bench.get("control_hits"):
        errors.append("inventory_control_hits_mismatch")
    for key in ("clock_step_ms", "quantization_bound_ms", "per_call_ns_resolution",
                "resolution_bound_ns_lower", "resolution_bound_ns_upper"):
        if not _number(bench.get(key), positive=True):
            errors.append("invalid_inventory_" + key)
    if bench.get("resolution_bound_kind") != "observed_clock_quantization_only":
        errors.append("inventory_resolution_bound_kind_mismatch")
    if errors:
        return errors
    # Serialized milliseconds have a 0.0001 ms step and ns costs a 0.1 ns
    # step. Propagate their half-step errors instead of inventing precision.
    bound = 2 * bench["pair_count"] * bench["clock_step_ms"]
    if not math.isclose(bench["quantization_bound_ms"], bound, rel_tol=0.,
                        abs_tol=(2 * bench["pair_count"] + 1) * .00005 + 1e-12):
        errors.append("inventory_quantization_bound_mismatch")
    resolution = bench["quantization_bound_ms"] * 1e6 / bench["total_calls"]
    if not math.isclose(bench["per_call_ns_resolution"], resolution, rel_tol=0.,
                        abs_tol=.05 + 50 / bench["total_calls"] + 1e-12):
        errors.append("inventory_per_call_resolution_mismatch")
    net, cost_bound = bench["per_call_ns_net"], bench["per_call_ns_resolution"]
    for key, value in (("resolution_bound_ns_lower", net - cost_bound),
                       ("resolution_bound_ns_upper", net + cost_bound)):
        if not math.isclose(bench[key], value, rel_tol=0., abs_tol=.15 + 1e-12):
            errors.append("inventory_resolution_interval_mismatch")
    if (bench["measured_loop_ms"] - bench["control_loop_ms"] <= bench["quantization_bound_ms"]
            or net <= cost_bound):
        errors.append("inventory_net_not_above_quantization_bound")
    return errors


def _validate_window(window):
    if not isinstance(window, dict):
        return ["not_object"]
    errors = []
    for key in ("frames", "blocks", "samples_kept"):
        if not _number(window.get(key), positive=True, integer=True):
            errors.append("invalid_" + key)
    for key in ("elapsed_s", "mean_ms", "median_ms", "p95_ms", "median_fps"):
        if not _number(window.get(key), positive=True):
            errors.append("invalid_" + key)
    if window.get("decimated") is not False:
        errors.append("incomplete_capture")
    if window.get("timing_source") != "entry_wall_clock":
        errors.append("unsupported_timing_source")
    if type(window.get("tail_coverage")) is not bool:
        errors.append("missing_tail_coverage")
    samples = window.get("samples_ms")
    if not isinstance(samples, list) or not samples or any(not _number(v, positive=True) for v in samples):
        errors.append("invalid_samples")
        return errors
    if len(samples) != window.get("samples_kept") or len(samples) != window.get("blocks"):
        errors.append("missing_samples")
    if all(_number(window.get(k), positive=True) for k in ("frames", "elapsed_s", "mean_ms")):
        if not math.isclose(window.get("mean_ms", 0), 1000 * window["elapsed_s"] / window["frames"], rel_tol=.002, abs_tol=.00001):
            errors.append("mean_total_mismatch")
    frames, elapsed = window.get("sample_frames"), window.get("sample_elapsed_s")
    if frames is not None or elapsed is not None:
        if (not isinstance(frames, list) or not isinstance(elapsed, list)
                or len(frames) != len(samples) or len(elapsed) != len(samples)
                or any(not _number(v, positive=True, integer=True) for v in frames)
                or any(not _number(v, positive=True) for v in elapsed)):
            errors.append("invalid_block_weights")
        else:
            if sum(frames) != window.get("frames") or not math.isclose(sum(elapsed), window.get("elapsed_s", 0), rel_tol=.002, abs_tol=max(.00001, len(samples) * .0000005)):
                errors.append("block_totals_mismatch")
            if any(not math.isclose(v, 1000 * t / n, rel_tol=.002, abs_tol=.00001) for v, t, n in zip(samples, elapsed, frames)):
                errors.append("block_mean_mismatch")
    ends = window.get("sample_end_s")
    if ends is not None and (not isinstance(ends, list) or len(ends) != len(samples)
                            or any(not _number(v, positive=True) for v in ends)
                            or any(b <= a for a, b in zip(ends, ends[1:]))):
        errors.append("unordered_samples")
    elif ends is not None and isinstance(elapsed, list) and len(elapsed) == len(ends) and all(_number(v, positive=True) for v in elapsed):
        cumulative = 0.
        for i, (end, duration) in enumerate(zip(ends, elapsed)):
            cumulative += duration
            if not math.isclose(end, cumulative, rel_tol=0., abs_tol=(i + 2) * .0000005):
                errors.append("noncontiguous_sample_timestamps")
                break
    return errors


def _mean(windows):
    return 1000 * sum(w["elapsed_s"] for w in windows) / sum(w["frames"] for w in windows)


def _block_length(values):
    """Positive-sequence correlation time, bounded by sqrt(n) and n/4.

    Selected separately per arm; report maximum for auditability. This is a
    pilot heuristic, not proof of interval coverage on an engine workload.
    """
    n = len(values)
    mean = statistics.fmean(values)
    denominator = sum((v - mean) ** 2 for v in values)
    if n < 4 or denominator <= 0:
        return 1
    tau = 1.
    for lag in range(1, min(n // 4 + 1, 101)):
        rho = sum((values[i] - mean) * (values[i + lag] - mean) for i in range(n - lag)) / denominator
        if rho <= 0:
            break
        tau += 2 * rho
    return max(1, min(math.ceil(tau), math.ceil(math.sqrt(n)), n // 4))


def _resampled_totals(window, rng, length, prefixes):
    n = len(window["samples_ms"])
    elapsed_prefix, frames_prefix = prefixes
    elapsed, frames, retained = 0., 0, 0
    while retained < n:
        start = rng.randrange(n - length + 1)
        take = min(length, n - retained)
        elapsed += elapsed_prefix[start + take] - elapsed_prefix[start]
        frames += frames_prefix[start + take] - frames_prefix[start]
        retained += take
    return elapsed, frames


def _quantile(values, proportion):
    ordered = sorted(values)
    position = (len(ordered) - 1) * proportion
    lower = int(position)
    return ordered[lower] + (ordered[min(lower + 1, len(ordered) - 1)] - ordered[lower]) * (position - lower)


def _bootstrap(groups):
    """Hierarchical bootstrap: paired repetitions outside, moving blocks inside.

    Each repetition is a tuple of reference window(s), treatment window(s).
    Bracketed A/A' is averaged equally. Block positions across sequential arms
    are independent; there is no implied framewise correspondence across arms.
    """
    windows = [w for group in groups for arm in group for w in arm]
    if any("sample_frames" not in w or "sample_elapsed_s" not in w for w in windows):
        return None, None, ["missing_block_weights"]
    if any(not isinstance(w.get("sample_end_s"), list) for w in windows):
        return None, None, ["missing_ordered_timestamps"]
    if len(groups) < 2 or any(len(w["samples_ms"]) < 8 for w in windows):
        return None, None, ["insufficient_repetitions_or_blocks"]
    lengths = {id(w): _block_length(w["samples_ms"]) for w in windows}
    prefixes = {id(w): (list(itertools.accumulate(w["sample_elapsed_s"], initial=0.)),
                        list(itertools.accumulate(w["sample_frames"], initial=0))) for w in windows}
    rng = random.Random(SEED)
    draws = []
    for _ in range(ITERATIONS):
        refs, targets = [], []
        for _ in groups:
            group = groups[rng.randrange(len(groups))]
            means = []
            for arm in group:
                arm_means = []
                for w in arm:
                    t, n = _resampled_totals(w, rng, lengths[id(w)], prefixes[id(w)])
                    arm_means.append(1000 * t / n)
                means.append(statistics.fmean(arm_means))
            refs.append(means[0])
            targets.append(means[1])
        ref, target = statistics.fmean(refs), statistics.fmean(targets)
        draws.append((target - ref, 1 - ref / target, 1000 / ref, 1000 / target))
    intervals = [[_quantile([draw[i] for draw in draws], .025),
                  _quantile([draw[i] for draw in draws], .975)] for i in range(4)]
    # Percentile bootstrap with very few repetitions can under-cover badly.
    # Conservatively widen its range using a small-sample t/normal ratio.
    # This safeguard is transparent and still does not qualify engine coverage.
    t95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571,
           6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228,
           11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131,
           16: 2.120, 17: 2.110, 18: 2.101, 19: 2.093, 20: 2.086,
           21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064, 25: 2.060,
           26: 2.056, 27: 2.052, 28: 2.048, 29: 2.045, 30: 2.042}
    inflation = t95.get(min(len(groups) - 1, 30), 2.042) / 1.96
    for interval in intervals:
        center = sum(interval) / 2
        half = (interval[1] - interval[0]) / 2 * inflation
        interval[:] = [center - half, center + half]
    metadata = {"method": "hierarchical_moving_block_percentile_small_sample_widened", "iterations": ITERATIONS,
                "seed": SEED, "confidence": .95, "repetitions": len(groups),
                "block_length": max(lengths.values()), "small_sample_inflation": inflation,
                "coverage": "simulation_only_unqualified"}
    return intervals, metadata, []


def _blank_report():
    return {"schema": "modperf-bench/analysis/1", "analyzer_version": ANALYZER_VERSION,
            "status": "invalid", "reasons": [], "baseline": None, "primitives": [],
            "rejected_benches": [], "independent_summaries": [],
            "stack_comparison": None,
            "capacity": {"status": "not_established", "population": None,
                         "reason": "requires_qualified_real_client_workload_and_restart_cycle_evidence"}}


def _primitives(results):
    groups = defaultdict(list)
    for b in results["benches"]:
        if b["protocol"] == "a_b_a_prime":
            groups[(b["id"], b.get("variant"), b["mechanism"], b["units"], b.get("period_ms"))].append(b)
    output = []
    for (bench_id, variant, mechanism, units, period), benches in groups.items():
        repetitions, deltas = [], []
        reasons = ["engine_timing_and_observer_not_qualified"]
        for bench in benches:
            w = bench["windows"]
            a, b, recovery = _mean([w["baseline"]]), _mean([w["treatment"]]), _mean([w["recovery"]])
            if abs(recovery - a) / a * 100 > results["config"]["a_prime_tolerance_pct"]:
                reasons.append("recovery_drift_exceeds_tolerance")
            deltas.append(b - (a + recovery) / 2)
            repetitions.append(((w["baseline"], w["recovery"]), (w["treatment"],)))
        intervals, metadata, uncertainty_reasons = _bootstrap(repetitions)
        reasons.extend(uncertainty_reasons)
        delta = statistics.fmean(deltas)
        interval = intervals[0] if intervals else None
        status = "estimated_unqualified"
        if "recovery_drift_exceeds_tolerance" in reasons:
            status, delta, interval = "invalid", None, None
        elif interval is None or interval[0] <= 0 <= interval[1] or delta <= 0:
            status = "unresolved"
        output.append({"id": bench_id, "variant": variant, "mechanism": mechanism, "units": units,
                       "period_ms": period, "status": status, "delta_ms": delta,
                       "per_unit_ns": delta * 1e6 / units if delta is not None and delta > 0 else None,
                       "interval_ms": interval, "bootstrap": metadata,
                       "unit": "elapsed_ms_per_engine_entry_frame", "reasons": sorted(set(reasons))})
    return output


def _compare(results, manifest, reference, reference_manifest):
    output = {"status": "not_comparable", "reasons": [], "delta_ms": None, "interval_ms": None,
              "fps_reference": None, "fps_stack": None, "fps_loss_fraction": None,
              "fps_loss_fraction_interval": None, "headroom_ms": None, "bootstrap": None}
    if validate(reference) or reference.get("schema") != "modperf-bench/results/2":
        output["reasons"].append("reference_invalid_or_historical")
    if not isinstance(manifest, dict) or not isinstance(reference_manifest, dict):
        output["reasons"].append("missing_manifests")
        return output
    for label, mf in (("target", manifest), ("reference", reference_manifest)):
        if mf.get("schema") != "modperf-bench/manifest/1":
            output["reasons"].append(label + ":unsupported_manifest_schema")
        for key in ("config_sha256", "world_sha256", "engine_sha256", "scenario_snapshot_sha256", "effective_config_comparison_sha256"):
            if not _hash(mf.get(key)):
                output["reasons"].append(label + ":invalid_hash:" + key)
        if mf.get("effective_config_normalization") != "mission-template-path/1":
            output["reasons"].append(label + ":unsupported_config_normalization")
        if mf.get("fps_limit") is not None and not _number(mf.get("fps_limit"), positive=True, integer=True):
            output["reasons"].append(label + ":invalid_fps_limit")
        if not isinstance(mf.get("mods"), list) or any(not isinstance(mod, dict) or not _hash(mod.get("sha256")) for mod in mf.get("mods", [])):
            output["reasons"].append(label + ":invalid_mod_identity")
    for label, data, mf in (("target", results, manifest), ("reference", reference, reference_manifest)):
        if not isinstance(data, dict) or not isinstance(data.get("config"), dict) or data["config"].get("scenario_id") != mf.get("scenario_id"):
            output["reasons"].append(label + ":scenario_manifest_mismatch")
    for key in MATCH_KEYS:
        if key not in manifest or key not in reference_manifest:
            output["reasons"].append("missing_identity:" + key)
        elif key != "fps_limit" and (manifest[key] in (None, "") or reference_manifest[key] in (None, "")):
            output["reasons"].append("unknown_identity:" + key)
        elif manifest[key] != reference_manifest[key]:
            output["reasons"].append("mismatched_identity:" + key)
    # The effective benchmark workload must match as well as the server config.
    a_config = {k: v for k, v in results["config"].items() if k not in ("manifest_sha256", "note", "hardware_note")}
    reference_config = reference.get("config") if isinstance(reference, dict) else None
    b_config = {k: v for k, v in (reference_config.items() if isinstance(reference_config, dict) else []) if k not in ("manifest_sha256", "note", "hardware_note")}
    if a_config != b_config:
        output["reasons"].append("mismatched_effective_config")
    if output["reasons"]:
        return output
    if manifest["fps_limit"] is not None:
        output.update(status="cap_limited", reasons=["frame_cap_can_conceal_stack_cost"])
        return output
    def baselines(data):
        return {b["replicate"]: b["windows"]["baseline"] for b in data["benches"] if b["protocol"] == "observed_baseline"}
    target, source = baselines(results), baselines(reference)
    if not target or target.keys() != source.keys():
        output["reasons"].append("missing_matched_baseline_repetitions")
        return output
    groups = [((source[k],), (target[k],)) for k in sorted(target)]
    intervals, metadata, reasons = _bootstrap(groups)
    ref = statistics.fmean(_mean([source[k]]) for k in source)
    stack = statistics.fmean(_mean([target[k]]) for k in target)
    output.update(status="estimated_unqualified", delta_ms=stack - ref,
                  fps_reference=1000 / ref, fps_stack=1000 / stack,
                  fps_loss_fraction=1 - ref / stack, bootstrap=metadata,
                  reasons=reasons + ["engine_timing_and_observer_not_qualified"])
    if intervals:
        output.update(interval_ms=intervals[0], fps_loss_fraction_interval=intervals[1],
                      fps_reference_interval=intervals[2], fps_stack_interval=intervals[3])
    if intervals is None or intervals[0][0] <= 0 <= intervals[0][1] or stack <= ref:
        output["status"] = "unresolved"
    return output


def analyze(results, manifest=None, reference=None, reference_manifest=None):
    """Analyze parsed evidence. Use analyze_files to bind external manifest bytes."""
    report = _blank_report()
    if isinstance(results, dict) and results.get("schema") == "modperf-bench/results/1":
        report.update(status="historical_unqualified", reasons=["legacy_protocol_not_reinterpreted"])
        return report
    errors = validate(results)
    report["reasons"] = [e for e in errors if not e.startswith("bench[")]
    if isinstance(manifest, dict) and isinstance(results, dict) and isinstance(results.get("config"), dict) and results["config"].get("scenario_id") != manifest.get("scenario_id"):
        report["reasons"].append("scenario_manifest_mismatch")
    if report["reasons"]:
        return report
    accepted = []
    for i, bench in enumerate(results["benches"]):
        failures = [e for e in errors if e.startswith(f"bench[{i}]:")]
        if failures:
            report["rejected_benches"].append({"id": bench.get("id") if isinstance(bench, dict) else None,
                                               "index": i, "reasons": failures})
        elif bench["protocol"] in ("observed_baseline", "a_b_a_prime"):
            accepted.append(bench)
        else:
            report["independent_summaries"].append({"id": bench["id"], "protocol": bench["protocol"],
                "status": "historical_summary_unqualified" if bench["protocol"] == "single_frame_loop" else "estimated_unqualified",
                "reported_per_call_ns_net": bench.get("per_call_ns_net"),
                "reason": "loop_evidence_does_not_establish_whole_frame_cost_or_population"})
    if not accepted:
        report["reasons"] = errors or ["no_valid_whole_frame_windows"]
        return report
    # Work with the independently accepted benches only; preserve exclusions.
    results = dict(results, benches=accepted)
    report["status"] = "observed_unqualified"
    report["reasons"] = ["engine_timing_and_observer_not_qualified"]
    clock = results.get("clock")
    if not isinstance(clock, dict) or type(clock.get("frame_pairing_valid")) is not bool or type(clock.get("observer_qualified")) is not bool:
        report["reasons"].append("clock_qualification_evidence_missing")
    if not manifest:
        report["reasons"].append("artifact_missing_manifest")
    windows = [b["windows"]["baseline"] for b in results["benches"]]
    if any(not isinstance(w.get("sample_end_s"), list) for w in windows):
        report["reasons"].append("missing_ordered_timestamps")
    mean = _mean(windows)
    report["baseline"] = {"mean_frame_ms": mean, "observed_rate_fps": 1000 / mean,
                          "frames": sum(w["frames"] for w in windows),
                          "elapsed_s": sum(w["elapsed_s"] for w in windows),
                          "individual_frame_tails_available": False,
                          "headroom_ms": None, "unit": "elapsed_engine_entry_gap",
                          "work_completion": None, "observer_cost_ms": None,
                          "independent_boots": None,
                          "block_mean_p50_ms": _quantile([v for w in windows for v in w["samples_ms"]], .5),
                          "block_mean_p95_ms": _quantile([v for w in windows for v in w["samples_ms"]], .95),
                          "distribution_unit": "retained_contiguous_block_mean_ms"}
    report["primitives"] = _primitives(results)
    if reference is not None:
        report["stack_comparison"] = _compare(results, manifest, reference, reference_manifest)
    return report


def _read(path):
    def reject(value):
        raise ValueError("nonfinite JSON constant: " + value)
    def unique(pairs):
        output = {}
        for key, value in pairs:
            if key in output:
                raise ValueError("duplicate JSON key: " + key)
            output[key] = value
        return output
    return json.loads(Path(path).read_text(encoding="utf-8-sig"), parse_constant=reject, object_pairs_hook=unique)


def analyze_files(results_path, manifest_path=None, reference_path=None, reference_manifest_path=None):
    """Strict JSON decoding plus SHA-256 of exact manifest bytes (including BOM)."""
    try:
        results = _read(results_path)
        reference = _read(reference_path) if reference_path else None
        manifest = _read(manifest_path) if manifest_path else None
        ref_manifest = _read(reference_manifest_path) if reference_manifest_path else None
        for data, path in ((results, manifest_path), (reference, reference_manifest_path)):
            if data is not None and path and data.get("schema") != "modperf-bench/results/1":
                expected = data.get("config", {}).get("manifest_sha256")
                digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
                if expected != digest or data.get("manifest_sha256") != digest:
                    report = _blank_report()
                    report["reasons"] = ["manifest_sha256_mismatch"]
                    return report
        return analyze(results, manifest, reference, ref_manifest)
    except (OSError, ValueError, TypeError, AttributeError) as exc:
        report = _blank_report()
        report["reasons"] = ["unreadable_or_invalid_artifact: " + str(exc)]
        return report
