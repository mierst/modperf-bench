# Operational baseline protocol

An operational baseline describes a particular host, map, starting world, mod
order and operating regime. It does not establish supported player count.
`BASELINE` records repeated observation windows without amplified benchmark
loads. Ordered block means describe elapsed entry-to-entry timing; they do not
certify individual frame stalls or CPU service time. Runtime qualification and
uncertainty analysis remain necessary even when structural validation passes.

## Windows preparation

Use a dedicated local server installation and an offline mission snapshot.
Never clone an actively changing production persistence directory. The mission
must include its economy and persistence files for a representative persisted
scenario. A pristine reference and a persisted scenario are separate runs.
Supply a small world manifest describing map, snapshot identity, intended
activation and counts where measured; do not put secrets in it.

```powershell
./tools/run-baseline.ps1 `
  -ServerRoot 'D:\BenchServer' `
  -ProfileDir 'D:\BenchRuns\reference-boot01' `
  -MissionSource 'D:\BenchSnapshots\reference.chernarusplus' `
  -ScenarioId 'reference-chernarus-v1' `
  -WorldManifestPath 'D:\BenchSnapshots\reference-world.json' `
  -ServerConfig 'D:\BenchInputs\server.cfg' `
  -ModPaths @('D:\BenchServer\@modperf-bench') `
  -HardwareNote 'Dedicated host; allocation and thermal context pending'
```

The default and explicit `-DryRun` prepare files without launching. Preparation
requires a fresh profile outside the installation and source snapshot, copies
the mission without changing it, verifies every copied file against the source,
and writes an isolated server config with its single mission template pointing
to the clone, preserving the mission terrain suffix. The generated config sets
`bindIP="127.0.0.1"`; launch also passes `-ip=127.0.0.1` for loopback binding.
Ambiguous templates and reparse-point snapshots are refused.
Preparation may leave an incomplete profile on an input failure; inspect it and
use a fresh directory after correction.

The `-mission` argument and generated mission template point at the clone.
Bohemia documents full mission paths in [custom terrain economy setup](https://community.bistudio.com/wiki/DayZ:Central_Economy_setup_for_custom_terrains).
Inspect startup RPT evidence to confirm the expected map and mission actually
loaded. A successful preparation or PBO package is not a script compilation or
mission activation check.

`-Run` explicitly requests a hidden dedicated server launch. The tool refuses
launch if any DayZ client or server process is running and checks again after
preparation. It never stops an existing process. A different game/session or
host contention can also affect results: run only on a controlled, idle host.
The returned PID identifies the launched server. Shut it down gracefully using
your normal server administration after collecting the run; the tool never
kills a process. `DurationSeconds` is the declared observation budget, not an
automatic termination timer or proof the run finished.

Defaults are 60 seconds warmup, three 60-second observation windows, two-second
transitions and 10 ms target blocks. Choose a longer duration budget when
increasing windows or repeats. Repeat independent boots with a new profile and
the same starting snapshot; windows in one boot are not independent boots.
Preserve startup and shutdown logs separately from steady-state observations.
Warmup and window durations accept 1–600 seconds, repeats 1–20, and target block
duration cannot exceed one tenth of the observation window. The launch adds RPT
logging but does not add admin/network logging. Keep logging settings fixed
across comparisons and qualify their overhead.

`ModPaths` is the ordered `-serverMod` list, including the benchmark PBO and
server-side dependencies. Client-required mod stacks need an appropriate manual
`-mod` launch and matching manifest; do not relabel server-only loading as a
complete client/server stack experiment. Mod/PBO content and order are hashed.

Use `-FpsLimit 50` for a separate operating-cap run, for example; use the default
zero for an uncapped request. Check effective delivered cadence and engine
configuration in either case. The requested cap alone cannot establish actual
cadence, spare work or zero mod cost. Do not combine capped and uncapped results.

## Evidence and privacy

Each profile retains `modperf-bench/config.json` and `manifest.json`. The manifest
uses `modperf-bench/manifest/1`; results use `modperf-bench/results/2` and protocol
`2.0`. Config binds the exact manifest bytes using SHA-256. The manifest records
engine/PBO/source identity, supplied config and world hashes, cloned scenario
fingerprint, ordered mod content, host identity, platform and requested cap.
`config_sha256` identifies the supplied server config; `effective_config_sha256`
identifies the generated local config whose absolute clone path differs between
boots. `effective_config_comparison_sha256` preserves all effective settings,
normalizes CRLF to LF and replaces only the single mission template value with
a canonical scenario placeholder. Its `effective_config_normalization` is
`mission-template-path/1`; scenario identity is compared separately. Changing
another effective option changes the comparison fingerprint. A dirty source
checkout is disclosed and is not identified by commit alone.

Raw server config and persistence are kept locally and are not included in the
manifest. Unknown CPU allocation, contention, thermal/power, storage, memory and
virtualization context are null with a reason. Record these separately before
qualifying a comparison. Hardware notes and mod labels are operator text: review
any artifact before sharing. This tooling performs no uploads.

After the engine writes results, retain both files together:

```powershell
./tools/validate-results.ps1 `
  -ResultsPath 'D:\BenchRuns\reference-boot01\modperf-bench\results.json' `
  -ManifestPath 'D:\BenchRuns\reference-boot01\modperf-bench\manifest.json' `
  -OutputDir 'D:\BenchEvidence\reference-boot01'
```

The validator checks schema, manifest binding, positive finite observations,
ordered retained-sample counts and basic window structure. It copies results
and the exact manifest to a fresh directory only after checks pass. Its result
is **STRUCTURALLY_VALID**, never a qualified capacity claim. Legacy results/1
remain historical evidence and are not copied as qualified v2 runs.

Failure verdicts and reasons remain in retained artifacts; a failed prerequisite
can have no dependent windows. B3 paired-loop observations are checked separately
from elapsed-frame windows. The validator exposes `invalid_rows` and never
qualifies those failures or converts B3 loop cost into whole-frame timing.
Statistical analysis must still assess missing/decimated observations, weighting, drift,
serial correlation, repeat-boot uncertainty, observer cost and clock pairing.

Qualify pristine and persisted worlds independently. Save cycles, idle-mode
entry/exit, active-world behavior and offered/completed workload require explicit
runtime evidence. An empty-world baseline cannot qualify persistence or real
connected-client behavior. Insufficient stability is inconclusive; no player
capacity number follows from this protocol.

## Linux manual launch

Keep Linux as a separately qualified platform. Use a complete isolated server
installation, a new profile and an unchanged offline source snapshot. Absolute
`-mission` behavior has a [reported Linux issue](https://feedback.bistudio.com/T195256),
so use a relative mission template under the isolated installation's mpmissions.
Do not use a live installation or modify its missions/config.

```bash
# Fill these with dedicated laboratory paths; refuse any existing run folder.
server=/srv/dayz-bench
snapshot=/srv/bench-snapshots/reference.chernarusplus
run=/srv/bench-runs/reference-boot01
pgrep -i '^DayZ' && { echo 'DayZ is running; refusing launch'; exit 1; }
test ! -e "$run" || exit 1
test ! -e "$server/mpmissions/mpb-reference-boot01.chernarusplus" || exit 1
mkdir -p "$run/modperf-bench" "$server/mpmissions"
cp -a "$snapshot" "$server/mpmissions/mpb-reference-boot01.chernarusplus"
cp /srv/bench-inputs/server.cfg "$run/server.cfg"
# Edit only the copied config: template="mpb-reference-boot01.chernarusplus";
# Set bindIP="127.0.0.1" and verify local binding in the startup evidence.
# Verify snapshot file hashes, and write the manifest/config before launching.
cd "$server"
./DayZServer -config="$run/server.cfg" -profiles="$run" -ip=127.0.0.1 \
  '-serverMod=@modperf-bench' -port=2402 -dologs >"$run/console.log" 2>&1 &
```

PowerShell 7 can run `write-run-manifest.ps1` and `validate-results.ps1` on Linux.
For the manifest, supply `-EngineExecutable DayZServer`, the original supplied
server config, `-EffectiveServerConfig` pointing at the copied config,
`-MissionSource` pointing at the cloned mission, and the same identity inputs
shown above. Write baseline config with the defaults above, `benches:["BASELINE"]`,
`scenario_id` and the returned exact `manifest_sha256`. If PowerShell is absent,
produce the same contract using SHA-256 tools; preserve ordered relative PBO
hashes and unknown context explicitly. Never reuse a Windows manifest for Linux.

Confirm mission activation and script compilation in the RPT, collect results
and manifest together, and use normal graceful administration to stop the
laboratory server. Repeat with fresh run and clone directories for every boot.
