# @modperf-bench

A server-side DayZ mod that measures, on a real server, the script-engine
costs a static cost model can only predict: what a live repeating timer costs
per frame just by existing, what firing one actually costs on top of that,
what an inventory attachment lookup costs per call, and what the common
"hook the busiest surface and throttle inside it" pattern costs relative to
just registering a slow timer.

It exists to make those numbers checkable rather than asserted. The
methodology this mod implements — an A/B/A′ protocol against a recovering
baseline — is described in full at https://dayz.fyi/modperf; this repository
is the reproducible instrument, not the write-up.

## What it is not

- Not a load tester and not a way to find out "how many players can this
  server take." It deliberately degrades frame time to make nanosecond-scale
  per-unit costs visible, which is the opposite of what you want on a server
  with anyone on it.
- Not a client mod. There is no client module, nothing player-identity-shaped
  is ever read, logged, or written, and nothing here talks to a network
  service.
- Not a general profiler. It measures four specific script-engine costs and
  nothing else; anything not in the bench suite below is out of scope.

## Safety

**Never run this on a live or populated server.** Load it only on a
throwaway or offline test instance. Every bench in the suite applies bulk
synthetic load — hundreds to thousands of no-op timers, or a hundred
thousand inventory lookups in a single frame — specifically to push a
per-unit cost that's normally too small to see up into a range a frame-time
distribution can resolve. That is not a side effect; it is how the
measurement works, and it comes at the cost of frame time for as long as a
run lasts (tens of minutes for the default config).

## Requirements and licensing

Server-side only. Ships unsigned — DayZ only checks mod signatures for
mods a client loads, and this mod never runs on a client, so no `.bikey` is
produced or needed. Licensed under the MIT License (see `LICENSE`).

## Building

Building needs Mikero's DePboTools (`MakePbo.exe`) installed on the machine
you run this on.

```powershell
powershell -File build\build.ps1
powershell -File build\build.ps1 -InstallTo "C:\path\to\DayZServer"
```

This packages `src\modperf_bench` into `build\@modperf-bench\addons\modperf_bench.pbo`.
`-InstallTo` additionally copies the finished `@modperf-bench` folder next to
a server executable, where a `-serverMod` folder is expected to live. A
successful build means MakePbo packaged the script files without complaint —
it does not mean the script compiles. The only real compile check is booting
a DayZ server with the mod loaded and confirming it comes up clean.

## Loading it

Add the mod to your **test** server's server-side mod list:

```
-serverMod=@modperf-bench
```

On Linux, use a path relative to the server's working directory — Linux DayZ
server builds silently ignore an absolute `-mod=`/`-serverMod=` path, which
looks identical to the mod not being loaded at all.

## Arming a run

The mod does nothing on its own. It is inert — no timers registered, no
frame samples collected, nothing written anywhere — unless
`$profile:modperf-bench/config.json` exists at boot. That file is the arming
switch: a server that has the mod loaded but no config file behaves exactly
as if the mod weren't there. This means the mod can safely stay on a
server's mod list between runs; nothing runs until you deliberately create
that file, and nothing keeps running once a suite finishes.

If the file is missing, the mod logs one line saying so and stops:

```
[modperf-bench] inert (no $profile:modperf-bench/config.json); create that file to arm a run
```

A run proceeds: settle → self-check → each configured bench in turn (its own
baseline → load → treatment → teardown → recovery), then the mod goes idle
again and writes its results file to
`$profile:modperf-bench/results-<utc-timestamp>.json`. Nothing needs to be
disarmed afterward; a finished run has already removed every timer and
accumulator it registered.

## Reading the log

Every meaningful line is prefixed `[modperf-bench]` in the server's script
log, and the same lines are mirrored into the results JSON's `log` array —
the log and the file can never disagree about what a run concluded. A
`BEGIN` line opens the run, a `WARNING` line restates the safety note above,
each bench prints one summary line ending in a verdict, and an `END` line
names the results file path.

## Bench suite

Every bench is windowed against amplified, bulk synthetic load (K identical
units, not one) because a single unit's cost is nanoscale — far below what a
frame-time distribution taken over one window can resolve. Dividing a
window's measured delta by K is what turns "invisible" into "measured."

- **B1 — timer residency.** K no-op repeating call-queue entries
  (`CallLater(fn, 0, true)`), K taken from `k_values` (100 / 500 / 1000 by
  default). Nothing the callback does is measured — the callback body is
  empty — only what it costs for the queue to carry K live entries that are
  due every tick. This is the only bench compared against a reference
  constant (`model_ns_timer_residency`).

- **B2 — firing cost vs residency.** The same amplified K timers (`b2_k`),
  run again at each period in `b2_periods_ms` (0 / 10 / 100 / 1000 ms by
  default). A period of 0 fires every tick; longer periods fire far less
  often but still cost the queue something to carry and check. The spread
  across periods is what separates "cost of being due" from "cost of merely
  existing."

- **B3 — inventory lookup.** The per-call cost of
  `GameInventory.FindAttachment`, the primitive behind "just look the
  attachment up" versus "cache the reference" — priced instead of asserted.
  One server-side entity is spawned, its slot and attachment are configured
  (see `b3_entity` / `b3_slot` / `b3_attachment` below — an empty slot name
  means the mod discovers a real occupied slot from the attachment itself
  rather than guessing a slot name), and a large batch of lookups runs
  inside a single frame, timed against an identical-length control loop that
  does trivial arithmetic instead. Both the raw and control-subtracted
  per-call cost are reported, along with whether the lookups were hitting an
  occupied slot — a lookup that hits and one that misses are not
  necessarily the same cost, and a bare number that didn't say which one it
  measured would not be trustworthy.

- **B5 — accumulator vs. dormant slow timer.** Prices the "hook the busiest
  per-frame surface and throttle inside it" pattern against the alternative
  of just registering one repeating call-queue entry at the interval you
  actually want. Two variants, each run with its own A/B/A′ windows: an
  every-frame no-op accumulator (`b5_units` parallel accumulator steps) and
  `b5_units` dormant 60-second timers. Both are expected — and usually
  found — to land below the noise floor for a single unit; what the pattern
  actually costs in the field is that same small residency cost paid by
  every mod that reaches for it, which is what B1's per-unit number prices.

- **Self-check (not a bench; a precondition for trusting the benches
  above).** Two short stages run before any bench: one counted timer, then K
  of them. Their fire-count ratio confirms that K registrations of one
  function really did become K independently-walked queue entries — if the
  engine had deduplicated them, every number above that divides by K would
  be wrong by that same factor. The same stages also measure how often the
  gameplay call queue is actually walked per second against how many engine
  frames pass per second — see the next section for why that number matters
  as much as the per-unit costs do.

## The queue-tick / engine-frame distinction

This is the one framing detail worth reading carefully before trusting a
"cost per frame" number out of this tool.

`CallLater` timers live on DayZ's gameplay call queue, and that queue is
walked once per **simulation tick**, not once per rendered **engine frame**.
On an idle server those two rates are not the same, and on a fast idle
server they can differ by more than an order of magnitude — the self-check
in the smoke example below measured roughly 1,000 queue ticks per second
against roughly 41,400 engine frames per second, a ratio of about 41 engine
frames per queue tick.

B1 and B2's per-unit numbers are measured against `timeslice` — engine frame
time — because that is the only timing source available without an engine
profiler (which is stubbed on retail server builds). That means the raw
per-unit-ns figure they report is a **per-frame** residency cost, not a
per-queue-tick cost. To get what one live timer costs each time the queue
that carries it is actually walked, multiply the per-frame figure by the
frames-per-queue-tick ratio the self-check measured on that same run. Both
numbers — the per-frame figure and the ratio — are written into the results
artifact together specifically so a reader doesn't have to take the divisor
on faith or guess it from a different server.

## Measurement honesty

Each windowed bench runs the same four-phase protocol:

**settle → A (baseline) → apply load → B (treatment) → teardown → A′ (recovery)**

- **A′ is not decoration.** Frame time drifts on a real box for reasons that
  have nothing to do with the load under test. Repeating the baseline
  conditions after teardown and checking that the server actually returned
  to them is the only way to tell a load's cost from ordinary drift, and the
  only proof that teardown genuinely removed the load. If A′ doesn't return
  to within `a_prime_tolerance_pct` of A, the run is marked
  `INVALID_A_PRIME` rather than shipping a number derived from drift.

- **Samples are frame-time blocks, not single frames**, each covering at
  least `block_seconds` (10 ms by default) of wall time. On a fast idle
  server, individual frame times are quantized — most report exactly zero
  with the accumulated remainder landing on an occasional frame — which
  makes a per-frame median meaningless while the mean stays fine. A block
  target expressed as a duration adapts on its own: at high frame rates a
  block covers many frames and quantization averages out inside it, and on
  a slow server where a single frame already exceeds the target, a block is
  just that one frame — the plain per-frame measurement, which is correct
  once frames are long enough to measure directly.

- **The noise floor is the larger of two numbers**, because either alone can
  be fooled: the recovery window's drift from baseline (which can land at
  exactly zero if A and A′ happen to quantize to the same value), and the
  baseline median's own standard error (which doesn't collapse the way
  drift can). A measured delta has to clear **twice** that noise floor
  before it's reported as a real effect at all — a delta that only edges
  past its own noise floor is exactly what a noise floor is bad at telling
  apart from noise.

- **Verdicts**, in the order they're decided:
  - `INVALID_A_PRIME` — the server didn't recover to baseline; the run
    isn't trustworthy and no cost figure from it should be used.
  - `BELOW_NOISE` — the measured delta didn't clear twice the noise floor.
    This is a legitimate, expected result for cheap effects (most of B5),
    not a failure.
  - `NEGATIVE_DELTA` — the treatment window measured *faster* than
    baseline by more than the noise floor. A load can only add work, so
    this means something other than the load moved the number; it is
    reported as its own verdict rather than folded into a model comparison.
  - `WITHIN_20PCT` / `DRIFT` — **B1 only**, the one bench with a reference
    constant (`model_ns_timer_residency`). The measured per-unit cost is
    compared to that constant; within 20% is `WITHIN_20PCT`, further off is
    `DRIFT` and is the signal that the reference constant should be
    revisited.
  - `MEASURED` — a significant delta with no reference constant to compare
    against (B2, B5's treatment variants). The number is real; there is
    simply nothing in the model yet to check it against.

- B3 isn't windowed the same way — it has no baseline/treatment/recovery
  because the whole measurement happens inside a single frame — but it
  applies the same honesty standard to its own timing source. The engine's
  wall clock (`GetGame().GetTickTime()`) advances in visible steps, and B3
  measures its own step size on the box it's running on (`clock_step_ms` in
  the output) rather than assuming a resolution, because that step size is
  the floor on how precisely a per-call figure can possibly be known on
  that server.

## Config keys

Every key `full-config.json` sets (all optional; a missing key falls back to
the mod's own default):

| Key | Meaning |
|---|---|
| `note` | Free-text label for the run, echoed into the results file. |
| `hardware_note` | Free-text description of the box the suite ran on. Written **verbatim** into the results artifact — fill it in yourself; nothing here inspects or reports actual hardware. |
| `settle_seconds` | How long the server free-runs before anything is measured, so the very first window isn't catching startup transients. |
| `window_seconds` | Length of each A/B/A′ window in a windowed bench. |
| `block_seconds` | Minimum wall-clock duration a frame-time sample (block) must cover; see "Measurement honesty" above. |
| `a_prime_tolerance_pct` | How close the recovery window has to land to the baseline window for a run to count as valid. |
| `model_ns_timer_residency` | The reference per-timer residency constant, in nanoseconds, that B1 alone is checked against. |
| `benches` | Which bench IDs to run this suite (`B1`, `B2`, `B3`, `B5`). |
| `k_values` | The K values B1 runs at (one full A/B/A′ run per value). |
| `b2_k` | The single K value B2 uses across all its periods. |
| `b2_periods_ms` | The timer periods, in milliseconds, B2 runs K timers at. |
| `b5_units` | Amplification factor for both B5 variants (accumulator steps and dormant slow timers). |
| `b3_lookups` | Lookups per repeat in B3's timed loop. |
| `b3_repeats` | How many times the lookup batch (and its control batch) repeats; the whole batch is timed as one measurement so it clears the wall clock's own step size — see B3 above. |
| `b3_entity` | Class name of the entity B3 spawns to hold the attachment under test. |
| `b3_fallback_entity` | Used if `b3_entity` fails to spawn; the run records whether a substitution happened. |
| `b3_slot` | Attachment slot name to look up. Empty string means "discover a real occupied slot automatically" — see B3 above for why that's the default rather than a guessed slot name. |
| `b3_attachment` | Class name of the attachment B3 creates in that slot. |
| `b3_position` | World position (`"x y z"`) the B3 subject entity is spawned at. |

## Output

A finished run writes `$profile:modperf-bench/results-<utc-timestamp>.json`,
schema-versioned (`"schema": "modperf-bench/results/1"`) so a downstream
reader can tell formats apart. It carries the run's config, the self-check
result, every bench's full window statistics and verdict, and the same log
lines printed to the server console. This file is the reproducible artifact
— attach it, archive it, or diff it against a later run on the same box.

A short excerpt, showing the self-check block described above:

```json
"self_check": {
  "name": "timer_multiplicity",
  "k": 100,
  "fires_with_1_entry": 1001,
  "fires_with_k_entries": 100100,
  "multiplicity_ratio": 100.0,
  "queue_ticks_per_second": 1000.0,
  "engine_frames_per_second": 41402.0,
  "engine_frames_per_queue_tick": 41.40,
  "note": "the gameplay call queue is ticked from the simulation update, not from every engine frame",
  "verdict": "K_ENTRIES_CONFIRMED"
}
```

## What's planned, not shipped

Sharing a results file back to build a public host-profile page, and
calibrating predictions per host from measured results, are both planned for
a future release; neither exists in this one — everything this mod does
stays on the machine it ran on until you choose to share the results file
yourself.
