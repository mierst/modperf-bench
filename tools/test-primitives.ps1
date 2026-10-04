param([switch]$AccumulatorOnly)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repoRoot 'src/modperf_bench/scripts/5_Mission'

# Execute the actual Enforce-compatible arithmetic and accumulator methods as
# C#. This is a language-subset fixture, not an Enforce compiler or engine smoke.
function Read-Method([string]$Source, [string]$Name) {
    $match = [regex]::Match($Source, '(?m)^\s*(?:private\s+)?(?:static\s+)?(?:bool|int|string|float|void)\s+' + $Name + '\([^\r\n]*\)\s*\{')
    if (-not $match.Success) { throw "Missing production method: $Name" }
    $depth = 1
    $end = $match.Index + $match.Length
    while ($depth -gt 0 -and $end -lt $Source.Length) {
        if ($Source[$end] -eq '{') { $depth++ }
        if ($Source[$end] -eq '}') { $depth-- }
        $end++
    }
    return $Source.Substring($match.Index, $end - $match.Index).Trim()
}
function Assert-Equal($Actual, $Expected, [string]$Reason) {
    if ($Actual -ne $Expected) { throw "$Reason : expected $Expected, got $Actual" }
}

$inventory = Get-Content -LiteralPath (Join-Path $sourceRoot 'MPB_Bench_InventoryLookup.c') -Raw
$accumulator = Get-Content -LiteralPath (Join-Path $sourceRoot 'MPB_Bench_Accumulator.c') -Raw
$methods = @()
if (-not $AccumulatorOnly) {
$methods = @('WorkIsBounded', 'BalancedPairCount', 'DeltaStatus') | ForEach-Object {
    (Read-Method $inventory $_) -replace '^static ', 'public static '
}
}
$constants = [regex]::Matches($inventory, '(?m)^\s*static const int MAX_[^;]+;') | ForEach-Object {
    $_.Value.Trim() -replace '^static const', 'public const'
}
$stateField = [regex]::Match($accumulator, '(?m)^\s*(?:ref array<float>|float)\s+m_Accumulated;').Value.Trim()
$stateField = $stateField -replace 'ref array<float>', 'EnforceArray<float>'
$accumulatorMethods = @('MPB_AccumulatorLoad', 'ApplyAccumulator', 'RemoveAccumulator', 'OnFrame', 'Crossings', 'SlowTick', 'ApplySlowTimer', 'RemoveSlowTimer', 'RemoveAll') | ForEach-Object {
    (Read-Method $accumulator $_) -replace '^(void|int)', 'public $1' -replace 'new array<float>', 'new EnforceArray<float>'
}
$accumulatorMethods = $accumulatorMethods -replace 'public void MPB_AccumulatorLoad', 'public AccumulatorFixture'
$accumulatorMethods = $accumulatorMethods -replace 'CALL_CATEGORY_GAMEPLAY', '0'
$code = @"
using System;
using System.Collections.Generic;
public class EnforceArray<T> : List<T> {
    public void Insert(T value) { Add(value); }
    public T Get(int index) { return this[index]; }
    public void Set(int index, T value) { this[index] = value; }
}
public class ScriptCallQueue {
    class Entry { public Action Callback; public long Due; public int Period; }
    readonly List<Entry> entries = new List<Entry>();
    long elapsed;
    public void CallLater(Action callback, int period, bool repeat) {
        if (!repeat || period < 1) throw new Exception("Expected repeating dormant call");
        entries.Add(new Entry { Callback = callback, Due = elapsed + period, Period = period });
    }
    public void Remove(Action callback) { entries.RemoveAll(entry => entry.Callback == callback); }
    public void Advance(int milliseconds) {
        elapsed += milliseconds;
        foreach (Entry entry in entries.ToArray()) {
            while (entry.Due <= elapsed) { entry.Callback(); entry.Due += entry.Period; }
        }
    }
}
public class QueueGame {
    public ScriptCallQueue Queue = new ScriptCallQueue();
    public ScriptCallQueue GetCallQueue(int category) { return Queue; }
}
public class PrimitivePolicy {
$($constants -join "`n")
$($methods -join "`n")
}
public class AccumulatorFixture {
    public QueueGame Game = new QueueGame();
    QueueGame GetGame() { return Game; }
    const float THRESHOLD_SECONDS = 60;
    bool m_Active;
    int m_Units;
    $stateField
    int m_Crossings;
    public int m_SlowTimerCount;
$($accumulatorMethods -join "`n")
}
"@
Add-Type -TypeDefinition $code

if (-not $AccumulatorOnly) {
Assert-Equal ([PrimitivePolicy]::WorkIsBounded(100000, 40)) $true 'Default work remains bounded'
Assert-Equal ([PrimitivePolicy]::WorkIsBounded(0, 40)) $false 'Zero lookup batch must not divide by zero'
Assert-Equal ([PrimitivePolicy]::WorkIsBounded(100000, 1)) $false 'One repeat cannot counterbalance'
Assert-Equal ([PrimitivePolicy]::WorkIsBounded(100000, 3)) $false 'Odd repeats cannot counterbalance'
Assert-Equal ([PrimitivePolicy]::WorkIsBounded(2147483647, 2147483646)) $false 'Reject amplification before integer overflow'
Assert-Equal ([PrimitivePolicy]::WorkIsBounded(1000000, 10)) $true 'Inclusive maximum work'
Assert-Equal ([PrimitivePolicy]::WorkIsBounded(1000000, 12)) $false 'Bound single-frame work'
Assert-Equal ([PrimitivePolicy]::BalancedPairCount(40)) 4 'Four equal pairs for default work'
Assert-Equal ([PrimitivePolicy]::BalancedPairCount(6)) 2 'Two equal pairs when four cannot divide'
Assert-Equal ([PrimitivePolicy]::BalancedPairCount(3)) 0 'Reject unbalanced order'
Assert-Equal ([PrimitivePolicy]::DeltaStatus(0, 0.002)) 'BELOW_RESOLUTION' 'Identical arms must not fabricate a cost'
Assert-Equal ([PrimitivePolicy]::DeltaStatus(0.001, 0.002)) 'BELOW_RESOLUTION' 'Positive sub-resolution effects are inconclusive'
Assert-Equal ([PrimitivePolicy]::DeltaStatus(0.002, 0.002)) 'BELOW_RESOLUTION' 'Resolution boundary is inconclusive'
Assert-Equal ([PrimitivePolicy]::DeltaStatus(-0.001, 0.002)) 'NEGATIVE_DELTA' 'Negative deltas never become measured'
Assert-Equal ([PrimitivePolicy]::DeltaStatus(0.005, 0.002)) 'MEASURED' 'Known positive effect clears clock uncertainty'
Assert-Equal ([PrimitivePolicy]::DeltaStatus(0.005, 0)) 'CLOCK_UNRESOLVED' 'Unknown clock cannot establish a measurement'
}

$load = [AccumulatorFixture]::new()
$load.ApplyAccumulator(3)
$load.OnFrame(20)
Assert-Equal $load.Crossings() 0 'Three units each receive 20 seconds, not a shared 60'
$load.OnFrame(20)
Assert-Equal $load.Crossings() 0 'Independent states have not crossed at 40 seconds'
$load.OnFrame(20)
Assert-Equal $load.Crossings() 3 'Every unit crosses once at 60 seconds'
$load.RemoveAccumulator()
$load.OnFrame(60)
Assert-Equal $load.Crossings() 3 'Removal stops all units'
$load.ApplyAccumulator(2)
$load.OnFrame(59)
Assert-Equal $load.Crossings() 0 'Reapply resets independent state'
$load.OnFrame(1)
Assert-Equal $load.Crossings() 2 'Reapplied states cross independently'
$load.RemoveAll()
$load.ApplySlowTimer(3)
$load.Game.Queue.Advance(60000)
Assert-Equal $load.Crossings() 0 'Dormant CallLater load must not fire at the accumulator threshold'
$load.RemoveAll()
$load.Game.Queue.Advance(86400000)
Assert-Equal $load.Crossings() 0 'Dormant CallLater teardown removes every callback'

if (-not $AccumulatorOnly) {
    # Run the complete B3 class with a deterministic clock and inventory at
    # the engine boundary. Method bodies and JSON builder remain production
    # code. This verifies pairing, counts and fail-closed output, not timing.
    $fullLookup = $inventory -replace 'class MPB_InventoryLookup', 'public class FullLookupFixture : LookupGameBase'
    $fullLookup = $fullLookup -replace 'void MPB_InventoryLookup\(', 'public FullLookupFixture('
    $fullLookup = $fullLookup -replace 'static const ', 'public const '
    $fullLookup = $fullLookup -replace 'ref array<float>', 'LookupArray<float>'
    $fullLookup = $fullLookup -replace 'new array<float>', 'new LookupArray<float>'
    $constructorStart = $fullLookup.IndexOf('public FullLookupFixture(')
    $lookupFields = $fullLookup.Substring(0, $constructorStart) -replace '(?m)^(\s+)(bool|string|int|float|LookupArray<float>)(\s+\w+;)', '$1public $2$3'
    $fullLookup = $lookupFields + $fullLookup.Substring($constructorStart)
    $fullLookup = $fullLookup -replace '(?m)^(\s+)(?:private )?(static )?(void|int|bool|string|float|EntityAI)(\s+\w+\()', '$1public $2$3$4'
    $fullLookup = $fullLookup -replace '\bObject\b', 'LookupObject' -replace '\bvector\b', 'Vector3'
    $fullLookup = $fullLookup -replace '\b(\d+\.\d+)\b', '${1}f'
    $fullLookup = $fullLookup -replace 'GetCurrentAttachmentSlotInfo\(attachedSlotId, attachedSlotName\)', 'GetCurrentAttachmentSlotInfo(out attachedSlotId, out attachedSlotName)'
    $lookupCode = @'
using System;
using System.Collections.Generic;
using System.Globalization;
public class LookupArray<T> : List<T> {
    public void Insert(T value) { Add(value); }
    public T Get(int index) { return this[index]; }
    public new int Count() { return base.Count; }
}
public class Vector3 {}
public class LookupObject {
    public static implicit operator bool(LookupObject value) { return value != null; }
}
public class EntityAI : LookupObject {
    public GameInventory Inventory;
    public static EntityAI Cast(LookupObject value) { return value as EntityAI; }
    public GameInventory GetInventory() { return Inventory; }
}
public class GameInventory {
    public int Calls;
    public EntityAI Result;
    public static implicit operator bool(GameInventory value) { return value != null; }
    public int GetSlotIdCount() { return 1; }
    public int GetSlotId(int index) { return 1; }
    public EntityAI FindAttachment(int slot) { Calls++; return Result; }
    public EntityAI CreateAttachment(string type) { return Result; }
    public EntityAI CreateAttachmentEx(string type, int slot) { return Result; }
    public bool GetCurrentAttachmentSlotInfo(out int id, out string name) { id = 1; name = "slot"; return true; }
}
public class InventorySlots {
    public const int INVALID = -1;
    public static int GetSlotIdFromString(string slot) { return 1; }
    public static string GetSlotName(int slot) { return "slot"; }
}
public class LookupGame {
    public float[] Ticks = new float[] { 0 };
    int read;
    public bool Hit = true;
    public int SpawnCount;
    public int DeleteCount;
    public GameInventory Inventory;
    public List<int> LookupCountsAtClockRead = new List<int>();
    public float GetTickTime() {
        if (LookupCountsAtClockRead.Count < 100) LookupCountsAtClockRead.Add(Inventory == null ? 0 : Inventory.Calls);
        if (read < Ticks.Length) return Ticks[read++];
        return Ticks[Ticks.Length - 1];
    }
    public LookupObject CreateObject(string type, Vector3 position, bool a, bool b, bool c) {
        SpawnCount++;
        EntityAI subject = new EntityAI();
        Inventory = new GameInventory { Result = Hit ? subject : null };
        subject.Inventory = Inventory;
        return subject;
    }
    public void ObjectDelete(LookupObject subject) { DeleteCount++; }
}
public class LookupGameBase {
    public LookupGame Game = new LookupGame();
    protected LookupGame GetGame() { return Game; }
}
public class MPB_Fmt {
    public static string Bool(bool value) { return value ? "true" : "false"; }
    public static string Quoted(string value) { return "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\""; }
    public static string Ms(float value) { return value.ToString("0.#########", CultureInfo.InvariantCulture); }
    public static string Ns(float value) { return Ms(value); }
}
'@ + $fullLookup
    Add-Type -TypeDefinition $lookupCode
    function New-LookupTicks([single]$ControlSeconds, [single]$LookupSeconds) {
        $ticks = [Collections.Generic.List[single]]::new()
        for ($probe = 0; $probe -le 9; $probe++) { $ticks.Add([single]($probe * 0.001)) }
        [single]$elapsed = 0.02
        for ($pair = 0; $pair -lt 4; $pair++) {
            $durations = @($ControlSeconds, $LookupSeconds)
            if ($pair % 2 -ne 0) { $durations = @($LookupSeconds, $ControlSeconds) }
            foreach ($duration in $durations) {
                $ticks.Add($elapsed)
                $elapsed += $duration
                $ticks.Add($elapsed)
            }
        }
        return $ticks.ToArray()
    }
    $lookup = [FullLookupFixture]::new()
    $lookup.Game.Ticks = New-LookupTicks 0.001 0.010
    $lookup.Run('fixture', '', 'slot', '', 100, 4, [Vector3]::new())
    $row = $lookup.ToJsonObject() | ConvertFrom-Json
    Assert-Equal $row.status 'MEASURED' 'Full B3 run accepts a positive resolved paired increment'
    Assert-Equal $row.total_calls 400 'Pairing preserves configured lookup budget'
    Assert-Equal $row.hits 400 'Every lookup result is consumed'
    Assert-Equal $row.control_hits 400 'Control consumes the same cached-result branch and sink'
    Assert-Equal $lookup.Game.Inventory.Calls 1402 'Warmup, slot probe and timing calls remain bounded'
    Assert-Equal ($row.pairs.order -join ',') 'control_lookup,lookup_control,control_lookup,lookup_control' 'JSON retains ordered pair evidence'
    $armCalls = @()
    for ($readIndex = 10; $readIndex -lt 26; $readIndex += 2) {
        $armCalls += $lookup.Game.LookupCountsAtClockRead[$readIndex + 1] - $lookup.Game.LookupCountsAtClockRead[$readIndex]
    }
    Assert-Equal ($armCalls -join ',') '0,100,100,0,0,100,100,0' 'Actual timed arms alternate equal lookup work'
    Assert-Equal $lookup.Game.DeleteCount 1 'B3 deletes its subject after timing'
    $lookup.Run('fixture', '', 'slot', '', 0, 4, [Vector3]::new())
    $invalidRow = $lookup.ToJsonObject() | ConvertFrom-Json
    Assert-Equal $invalidRow.status 'INVALID_WORK' 'Invalid work fails before spawn'
    Assert-Equal $lookup.Game.SpawnCount 1 'Invalid re-run does not allocate an entity'
    Assert-Equal $invalidRow.slot_id -1 'Invalid re-run must not retain a previous subject slot'
    foreach ($case in @(
        @{ Control = 0.002; Lookup = 0.002; Status = 'BELOW_RESOLUTION' },
        @{ Control = 0.002; Lookup = 0.001; Status = 'NEGATIVE_DELTA' }
    )) {
        $failedLookup = [FullLookupFixture]::new()
        $failedLookup.Game.Ticks = New-LookupTicks $case.Control $case.Lookup
        $failedLookup.Run('fixture', '', 'slot', '', 100, 4, [Vector3]::new())
        $failedRow = $failedLookup.ToJsonObject() | ConvertFrom-Json
        Assert-Equal $failedRow.status $case.Status 'Full B3 run propagates inconclusive effects'
        Assert-Equal $failedRow.per_call_ns_net $null 'Inconclusive effects cannot publish a priced lookup'
        Assert-Equal ($failedRow.validity_reasons -join ',') $case.Status 'Inconclusive reason is retained'
        Assert-Equal $failedLookup.Game.DeleteCount 1 'Inconclusive timing tears down its subject'
    }
    $stalledLookup = [FullLookupFixture]::new()
    $stalledLookup.Run('fixture', '', 'slot', '', 100, 4, [Vector3]::new())
    $stalledRow = $stalledLookup.ToJsonObject() | ConvertFrom-Json
    Assert-Equal $stalledRow.status 'CLOCK_UNRESOLVED' 'Stalled clock fails closed after bounded probe'
    Assert-Equal $stalledRow.clock_step_ms $null 'Unresolved clock step is unknown rather than zero'
    Assert-Equal $stalledRow.resolution_bound_ns_upper $null 'No timing pairs means no numeric resolution bound'
    Assert-Equal $stalledLookup.Game.DeleteCount 1 'Unresolved clock tears down its subject'

    $timerSource = Get-Content -LiteralPath (Join-Path $sourceRoot 'MPB_Bench_TimerResidency.c') -Raw
    $nativeStart = $timerSource.IndexOf('class MPB_NativeTimerLoad')
    if ($nativeStart -lt 0) { throw 'Missing production class: MPB_NativeTimerLoad' }
    $nativeSource = $timerSource.Substring($nativeStart)
    $nativeSource = $nativeSource -replace 'class MPB_NativeTimerLoad', 'public class NativeTimerFixture'
    $nativeSource = $nativeSource -replace 'void MPB_NativeTimerLoad\(', 'public NativeTimerFixture('
    $nativeSource = $nativeSource -replace 'static const ', 'public const '
    $nativeSource = $nativeSource -replace 'ref array<ref Timer>', 'NativeArray<Timer>'
    $nativeSource = $nativeSource -replace 'new array<ref Timer>', 'new NativeArray<Timer>'
    $nativeSource = $nativeSource -replace 'CALL_CATEGORY_GAMEPLAY', '0'
    $nativeSource = $nativeSource -replace '(?m)^(\s+)(void|int|bool) (\w+\()', '$1public $2 $3'
    $nativeSource = $nativeSource -replace '\.Count\(\)', '.Count'
    $nativeSource = $nativeSource -replace '\b(\d+\.\d+)\b', '${1}f'
    $nativeCode = @"
using System;
using System.Collections.Generic;
public class NativeArray<T> : List<T> {
    public void Insert(T value) { Add(value); }
    public T Get(int index) { return this[index]; }
}
public class Timer {
    public static int LiveCount;
    public static int CreatedCount;
    public static int FailOnCreation;
    int creationIndex;
    bool running;
    public Timer(int category) {
        if (category != 0) throw new Exception("Wrong Timer category");
        CreatedCount++;
        creationIndex = CreatedCount;
    }
    public void Run(float period, object target, string method, object args, bool repeat) {
        if (period < 86400 || !repeat || method != "CountedTick" || args != null)
            throw new Exception("Native residency Timer must be dormant and counted");
        if (target.GetType().GetMethod(method) == null) throw new Exception("Callback missing");
        if (creationIndex == FailOnCreation) return;
        if (!running) LiveCount++;
        running = true;
    }
    public bool IsRunning() { return running; }
    public void Stop() { if (running) LiveCount--; running = false; }
}
$nativeSource
"@
    Add-Type -TypeDefinition $nativeCode
    $nativeLoad = [NativeTimerFixture]::new()
    $nativeLoad.Apply(3)
    Assert-Equal $nativeLoad.ActiveCount() 3 'Native units retain distinct running Timer objects'
    Assert-Equal ([Timer]::CreatedCount) 3 'Amplification constructs one native Timer per unit'
    Assert-Equal $nativeLoad.IsDormant() $true 'Native timers begin with zero fires'
    $nativeLoad.CountedTick()
    Assert-Equal $nativeLoad.IsDormant() $false 'Any native callback invalidates residency'
    $nativeLoad.Remove()
    Assert-Equal ([Timer]::LiveCount) 0 'Native teardown stops every retained timer'
    Assert-Equal $nativeLoad.ActiveCount() 0 'Native teardown clears retained registrations'
    Assert-Equal $nativeLoad.FireCount() 1 'Native teardown retains invalidating callback evidence'
    $nativeLoad.Apply(2)
    Assert-Equal $nativeLoad.FireCount() 0 'New native treatment resets callback count'
    $nativeLoad.Apply(4)
    Assert-Equal ([Timer]::LiveCount) 4 'Reapply removes old native timers'
    $nativeLoad.Apply(2147483647)
    Assert-Equal ([Timer]::LiveCount) 0 'Over-budget native registrations fail before allocation'
    [Timer]::FailOnCreation = [Timer]::CreatedCount + 2
    $nativeLoad.Apply(3)
    Assert-Equal $nativeLoad.ActiveCount() 0 'Partial native registration failure rejects the treatment'
    Assert-Equal ([Timer]::LiveCount) 0 'Partial native registration failure tears down earlier timers'
    $nativeLoad.Remove()
}
Write-Output 'B3 paired-run/output, primitive arithmetic, accumulator and timer lifecycle checks passed (Enforce-compatible subset; engine smoke still required).'
