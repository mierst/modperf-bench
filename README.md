# modperf-bench

A server-side DayZ benchmark for host baselines and specific script-engine
mechanisms. Version 2 separates observed results from qualified calibration.
It does not estimate player capacity from idle FPS.

**Use only an isolated, unpopulated test instance.** Primitive benches apply
synthetic load deliberately. The baseline bench applies no synthetic load,
but it still requires an isolated scenario for a reproducible comparison.

## Build and load

Building requires Mikero DePboTools:

```powershell
powershell -ExecutionPolicy Bypass -File build\build.ps1
```

The packaged mod is `build/@modperf-bench`. Load it with
`-serverMod=<path-to-mod>`. A green package is not an Enforce compilation
check; boot a real dedicated server and inspect its fresh script log and RPT.
On Linux, use relative mod paths and qualify timing separately.

The mod is disarmed unless `<profiles>/modperf-bench/config.json` exists.
Its mission override remains installed while disarmed; it creates no timers
or samples. Removing the arming file prevents the next boot from running.
Finishing a suite removes its loads, but the file remains armed for the next boot.

## Operational baseline

Prepare a dedicated profile and a copy of a known mission/world snapshot:

```powershell
.\tools\run-baseline.ps1 `
  -ServerRoot 'C:\DayZServer' `
  -ProfileDir 'C:\bench-runs\reference-01' `
  -MissionSource 'C:\DayZServer\mpmissions\dayzOffline.chernarusplus' `
  -ScenarioId 'chernarus-reference' `
  -WorldManifestPath 'C:\snapshots\reference-manifest.json' `
  -ServerConfig 'C:\DayZServer\serverDZ.cfg' `
  -ModPaths 'C:\mods\@modperf-bench' `
  -DryRun
```

Preparation is the default: it clones the mission and writes a launch manifest
and effective config. `-Run` explicitly starts the dedicated server hidden;
it refuses an existing DayZ client/server and never stops another process.
Use a fresh profile for each run. The server must be shut down separately;
`DurationSeconds` is a planning budget, not an automatic stop timer.

A world manifest describes the intended scenario; the tooling hashes it and
the copied mission tree but does not assert that the workload was activated.
A pristine mission and a representative persisted world are different scenarios.
See [Operational baseline](docs/operational-baseline.md) for Windows/Linux
recipes, identity, cap handling and interpretation.

## Primitive suite

Arm the isolated server using `build/smoke-config.json` or
`build/full-config.json`. All effective options are echoed in the v2 result.

| ID | Treatment | Interpretation |
|---|---|---|
| BASELINE | No synthetic treatment | Observed host frame-gap distribution |
| B0 | Distinct native Timer objects, due after 24 hours | Never-firing native Timer residency; any firing invalidates the run |
| B1 | K empty CallLater callbacks due every gameplay queue tick | Residency plus firing, not pure dormant residency |
| B2 | Same CallLater load at specified periods | Period-specific costs; do not assume queue ticks equal engine frames |
| B3 | Alternating-order lookup/control batch pairs | Net inventory lookup loop cost, resolution-qualified locally |
| B5 | Independent accumulators versus 24-hour CallLater entries | Amplified per-frame accumulator versus dormant queue treatment |

No chain-dispatch calibration is included. The old 140 ns native Timer
reference is not compared with due CallLater entries. All model comparison
fields in v2 windowed runs remain null pending a mechanism-matched calibration.

Windowed runs use repeated A/B/A-prime contrasts, alternate ladder order,
and discard configurable transition time. CallLater results require a
successful timer-multiplicity self-check; failures invalidate dependent rows.
Recovery failure invalidates the affected run. Native Timer registration and
never-firing checks are independent gates. B3 preserves original total work
while splitting it into balanced pairs; invalid amplification, unresolved
clock precision and negative net results cannot produce a trusted net value.

## Config keys

The arming file is `<profiles>/modperf-bench/config.json`. Copy
[smoke-config.json](build/smoke-config.json) for short diagnostic windows or
[full-config.json](build/full-config.json) for the primitive suite, then edit
the copy for your test instance. Both files remain supported. For baseline-only
capture, set `benches` to `["BASELINE"]`; the preparation tool generates this
configuration and its companion manifest for you.

All keys are optional once the arming file exists. Omitted keys use the runner
defaults below; the result records the effective values. An empty config runs
the default primitive suite, so use an explicit bench list.

| Key | Default | Meaning |
|---|---|---|
| `note` | `""` | Free-text run label. |
| `hardware_note` | `""` | Operator-supplied description, echoed into results; not hardware detection. |
| `scenario_id` | `"unspecified"` | Scenario identity matched against the companion manifest. |
| `manifest_sha256` | `""` | SHA-256 of the exact companion manifest bytes; generated by preparation tools. |
| `settle_seconds` | `120` | Startup settling time before measurement, bounded to 1–600 seconds. |
| `window_seconds` | `60` | Duration of each baseline/treatment/recovery window, bounded to 1–600 seconds. |
| `block_seconds` | `0.01` | Target observation-block duration; nonpositive values reset to the default and values above one tenth of the window are reduced. |
| `repeats` | `3` | Replicates of each configured run; allowed range 1–20. |
| `transition_seconds` | `2` | Unmeasured settling time between phases; allowed range 0–120 seconds. |
| `a_prime_tolerance_pct` | `10` | Allowed baseline/recovery median difference, 0–100 percent. |
| `run_self_tests` | `0` | Set to `1` to run opt-in engine fixtures before the suite. |
| `model_ns_timer_residency` | `0` | Legacy provenance field, still accepted and echoed; v2 does not use it to price or classify a bench. |
| `benches` | `["B0","B1","B2","B3","B5"]` | Bench selection; `BASELINE` is also supported. |
| `k_values` | `[100,500,1000]` | Unit counts for B0 and B1; at most 20 values, each 1–10,000. |
| `b2_k` | `500` | CallLater registrations for each B2 period, 1–10,000. |
| `b2_periods_ms` | `[0,10,100,1000]` | B2 periods in milliseconds; at most 20 values, each 0–86,400,000. Zero means due every queue tick. |
| `b5_units` | `1000` | Independent accumulators or dormant CallLater registrations for B5, 1–10,000. |
| `b3_lookups` | `100000` | Lookups per repeat in each B3 arm. Invalid amplification produces an invalid row. |
| `b3_repeats` | `40` | Total repeat amplification, split across balanced lookup/control pairs; must be even and satisfy the B3 work bounds. |
| `b3_entity` | `"PlateCarrierVest"` | Subject entity class for B3. |
| `b3_fallback_entity` | `"PlateCarrierVest"` | Fallback if the preferred entity cannot spawn; substitution is recorded. |
| `b3_slot` | `""` | Attachment slot; empty means discover a real occupied slot. |
| `b3_attachment` | `"PlateCarrierHolster"` | Attachment class for B3. |
| `b3_position` | `"1000 400 1000"` | Subject world position as an `"x y z"` string. |

Out-of-range repeat, transition, unit and period values are reset rather than
accepted as requested: invalid `k_values` entries become `100`, and invalid
`b2_periods_ms` entries become `1000`; other listed values use their defaults.
Check the echoed config.
B3 applies its own work-validation gate instead of silently correcting its
amplification. The smoke example exercises the suite; short windows do not
qualify a calibration.

## Results and analysis

Results are written locally to:

```text
<profiles>/modperf-bench/results-<utc-timestamp>.json
```

Schema `modperf-bench/results/2` records the complete effective configuration,
protocol version, manifest digest, repeated contrasts, verdicts and ordered
block means with frame/elapsed weights. Median/p95 refer to **block means**,
not individual frame tails. Capture is bounded; decimation is disclosed and
blocks precision claims. Entry-gap timing covers elapsed delay, not CPU time.

The preparation tool writes `manifest.json` beside the arming config. It
contains source/build, ordered PBO, engine, configuration and scenario hashes.
A manually armed run without a companion manifest remains descriptive.

Analyze with Python 3.11+; the analyzer uses the standard library only:

```text
python tools/analyze-results.py RESULTS.json --manifest manifest.json --output analysis.json
```

For a matched reference/target comparison:

```text
python tools/analyze-results.py TARGET.json --manifest TARGET-manifest.json --reference REFERENCE.json --reference-manifest REFERENCE-manifest.json --output comparison.json
```

The analyzer checks exact manifest bytes and scenario/config/host identities,
resamples repeated contrasts with moving blocks, preserves unknowns and
rejects invalid evidence. It reports frame-time tax before deriving FPS loss.
Capped runs cannot reveal tax from equal FPS alone. A comparison requires
matched scenarios; it does not make individual mod costs additive.

**All current v2 results remain observed or estimated and unqualified.** Real
engine null/positive controls, observer-overhead qualification, repeated boots
and platform qualification are required before treating them as portable
constants. The uncertainty algorithm has synthetic tests; these do not certify
its coverage on a real server. Player capacity remains `not_established`.
Version 1 examples in `build/` retain historical interpretation and are never
silently upgraded by the analyzer.

## Website workflow

Use the [modperf website](https://dayz.fyi/modperf) for static stack analysis.
The v2 files can be archived alongside that report, and the local analyzer
can compare matching runs before or after a stack change. There is currently
no benchmark-result upload or automatic calibration import on the website.
No benchmark data is sent over the network by this mod or these tools.

A population estimate requires representative connected-client workloads,
network/work-completion and frame-tail evidence across the intended restart
interval. High idle FPS does not supply that evidence.

## Tests

```text
python -m unittest discover -s tests -v
```

```powershell
powershell -ExecutionPolicy Bypass -File tools\test-primitives.ps1
powershell -ExecutionPolicy Bypass -File tools\test-baseline-tools.ps1
powershell -ExecutionPolicy Bypass -File tools\test-runner.ps1
```

The primitive test executes a compatible source subset with test engine types;
it is not an Enforce compiler. Opt-in engine fixtures use `run_self_tests: 1`.
Read fresh script/RPT logs after a real boot. Windows and Linux qualification
are separate requirements.

MIT licensed; server-side only. The source checkout may be newer than the
[Steam Workshop item](https://steamcommunity.com/sharedfiles/filedetails/?id=3797721326).
