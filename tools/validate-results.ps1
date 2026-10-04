[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ResultsPath,
    [string]$ManifestPath = '',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
function Require($Condition,[string]$Message) { if (-not $Condition) { throw "Invalid results: $Message" } }
function PositiveNumber($Value,[string]$Label) {
    Require ($null -ne $Value -and $Value -is [ValueType] -and $Value -isnot [bool]) "$Label missing or not numeric"
    $n = [double]$Value
    Require (-not [double]::IsNaN($n) -and -not [double]::IsInfinity($n) -and $n -gt 0) "$Label must be finite and positive"
}
function FiniteNumber($Value,[string]$Label) {
    Require ($null -ne $Value -and $Value -is [ValueType] -and $Value -isnot [bool]) "$Label missing or not numeric"
    $n = [double]$Value
    Require (-not [double]::IsNaN($n) -and -not [double]::IsInfinity($n)) "$Label must be finite"
}
function NonnegativeInteger($Value,[string]$Label) {
    FiniteNumber $Value $Label
    Require ([double]$Value -ge 0 -and [double]$Value -eq [Math]::Floor([double]$Value)) "$Label must be a nonnegative integer"
}
$r = Get-Content -LiteralPath $ResultsPath -Raw | ConvertFrom-Json
Require ($r.schema -in @('modperf-bench/results/1','modperf-bench/results/2')) 'unsupported schema'
Require ($null -ne $r.benches -and @($r.benches).Count -gt 0) 'missing benches (interrupted or empty run)'
if ($r.schema -eq 'modperf-bench/results/1') {
    Require ([string]::IsNullOrEmpty($OutputDir)) 'historical v1 cannot be copied as qualified v2 evidence'
    [pscustomobject]@{schema=$r.schema;status='HISTORICAL_V1';qualified=$false;reason='Legacy evidence retains original interpretation; v2 qualification unavailable.'}
    return
}
Require ($r.protocol_version -eq '2.0') 'unsupported protocol'
Require ($null -ne $r.config) 'missing complete effective config'
Require (-not [string]::IsNullOrEmpty($ManifestPath)) 'v2 requires companion manifest'
$m = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
Require ($m.schema -eq 'modperf-bench/manifest/1') 'unsupported manifest schema'
$hash = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
Require ($r.manifest_sha256 -eq $hash -and $r.config.manifest_sha256 -eq $hash) 'manifest digest mismatch'
foreach ($field in @('scenario_id','config_sha256','world_sha256','engine_sha256','host_id','platform','observer_mode')) {
    Require (-not [string]::IsNullOrEmpty([string]$m.$field)) "manifest $field missing"
}
foreach ($field in @('config_sha256','world_sha256','engine_sha256','scenario_snapshot_sha256','host_id')) { Require ($m.$field -match '^[0-9a-fA-F]{64}$') "manifest $field not SHA256" }
Require ($m.effective_config_sha256 -match '^[0-9a-fA-F]{64}$') 'exact effective config SHA256 missing'
Require ($m.effective_config_comparison_sha256 -match '^[0-9a-fA-F]{64}$') 'effective config comparison SHA256 missing'
Require ($m.effective_config_normalization -eq 'mission-template-path/1') 'unsupported effective config normalization'
Require ($null -ne $m.mods -and @($m.mods).Count -gt 0) 'ordered mod identities missing'
foreach ($mod in $m.mods) {
    Require ($mod.sha256 -match '^[0-9a-fA-F]{64}$') 'mod identity not SHA256'
    Require (-not [string]::IsNullOrEmpty($mod.label)) 'mod label missing'
}
Require ($r.config.scenario_id -eq $m.scenario_id) 'scenario identity mismatch'
Require ($null -ne $r.clock) 'clock qualification metadata missing'
$invalidRows = 0
foreach ($bench in $r.benches) {
    Require (-not [string]::IsNullOrEmpty($bench.id)) 'bench id missing'
    Require ($bench.protocol -in @('observed_baseline','a_b_a_prime','paired_loops')) 'unknown observation protocol'
    Require ($bench.verdict -is [string] -and -not [string]::IsNullOrEmpty($bench.verdict)) 'bench verdict missing'
    Require ($bench.validity_reasons -is [array]) 'bench validity reasons must be an array'
    foreach ($reason in $bench.validity_reasons) { Require ($reason -is [string] -and -not [string]::IsNullOrEmpty($reason)) 'bench validity reason must be nonempty text' }
    $failed = $bench.verdict -like 'INVALID_*' -or @($bench.validity_reasons).Count -gt 0
    if ($failed) { $invalidRows++ }
    if ($bench.protocol -eq 'paired_loops') {
        Require ($bench.id -eq 'B3') 'paired_loops must identify B3'
        if ($null -ne $bench.pairs) {
            NonnegativeInteger $bench.pair_count 'B3 pair_count'
            Require ($bench.pair_count -eq @($bench.pairs).Count) 'B3 pair count mismatch'
            $pairIndex = 0
            foreach ($pair in $bench.pairs) {
                NonnegativeInteger $pair.index 'B3 pair index'
                Require ($pair.index -eq $pairIndex) 'B3 pairs must retain contiguous order'
                Require ($pair.order -in @('control_lookup','lookup_control')) 'B3 pair order invalid'
                PositiveNumber $pair.calls 'B3 calls'
                Require ([double]$pair.calls -eq [Math]::Floor([double]$pair.calls)) 'B3 calls must be integral'
                foreach ($field in @('control_ms','lookup_ms','delta_ms')) { FiniteNumber $pair.$field "B3 $field" }
                if (-not $failed) { Require ($pair.control_ms -ge 0 -and $pair.lookup_ms -ge 0) 'successful B3 loop durations cannot be negative' }
                $pairIndex++
            }
        }
        if (-not $failed) { Require ($null -ne $bench.pairs -and @($bench.pairs).Count -gt 0) 'B3 successful row lacks paired observations' }
        continue
    }
    $names = @('baseline')
    if ($bench.protocol -eq 'a_b_a_prime') { $names = @('baseline','treatment','recovery') }
    if ($failed) {
        Require (@($bench.validity_reasons).Count -gt 0) 'failed bench must preserve a validity reason'
        # Failed prerequisites can prevent all dependent windows. Validate any
        # observations that were emitted, while retaining the failure unchanged.
        $names = @($names | Where-Object { $null -ne $bench.windows.$_ })
    }
    foreach ($name in $names) {
        $w = $bench.windows.$name
        Require ($null -ne $w) "missing $name window"
        PositiveNumber $w.frames "$name frames"
        PositiveNumber $w.blocks "$name blocks"
        Require ([double]$w.frames -eq [Math]::Floor([double]$w.frames)) 'frames must be integral'
        Require ([double]$w.blocks -eq [Math]::Floor([double]$w.blocks)) 'blocks must be integral'
        PositiveNumber $w.mean_ms "$name mean_ms"
        PositiveNumber $w.elapsed_s "$name elapsed_s"
        Require ($null -ne $w.samples_ms -and @($w.samples_ms).Count -gt 0) "$name ordered samples missing"
        NonnegativeInteger $w.samples_kept "$name samples_kept"
        Require ($w.samples_kept -eq @($w.samples_ms).Count) "$name retained count mismatch"
        Require ($w.samples_kept -le $w.blocks) "$name retained count exceeds total blocks"
        Require ($null -ne $w.decimated -and $w.decimated -is [bool]) "$name decimation disclosure missing"
        Require ($w.timing_source -eq 'entry_wall_clock') "$name unsupported timing source"
        Require ($w.tail_coverage -is [bool]) "$name tail coverage disclosure missing"
        foreach ($sample in $w.samples_ms) { PositiveNumber $sample "$name sample" }
        if ($null -ne $w.sample_frames -or $null -ne $w.sample_elapsed_s) {
            Require (@($w.sample_frames).Count -eq @($w.samples_ms).Count -and @($w.sample_elapsed_s).Count -eq @($w.samples_ms).Count) "$name sample weights mismatch"
            foreach ($weight in $w.sample_frames) {
                PositiveNumber $weight "$name sample frames"
                NonnegativeInteger $weight "$name sample frames"
            }
            foreach ($elapsed in $w.sample_elapsed_s) { PositiveNumber $elapsed "$name sample elapsed" }
        }
    }
}
if (-not [string]::IsNullOrEmpty($OutputDir)) {
    $dest = [IO.Path]::GetFullPath($OutputDir)
    if (Test-Path -LiteralPath $dest) { throw 'Evidence output already exists; choose a fresh directory.' }
    New-Item -ItemType Directory -Path $dest | Out-Null
    Copy-Item -LiteralPath $ResultsPath -Destination (Join-Path $dest 'results.json')
    Copy-Item -LiteralPath $ManifestPath -Destination (Join-Path $dest 'manifest.json')
}
[pscustomobject]@{schema=$r.schema;status='STRUCTURALLY_VALID';qualified=$false;invalid_rows=$invalidRows;manifest_sha256=$hash;reason='Structure and identity checked only; failed rows retained without qualification. Statistical, observer, frame-pairing and operating-capacity qualification require analysis and engine evidence.'}
