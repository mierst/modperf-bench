$ErrorActionPreference = 'Stop'
function Assert($Condition, $Message) { if (-not $Condition) { throw "FAIL: $Message" } }
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('mpb-tools-' + [guid]::NewGuid())
New-Item -ItemType Directory $fixture | Out-Null
try {
    $server = New-Item -ItemType Directory (Join-Path $fixture 'server')
    $mission = New-Item -ItemType Directory (Join-Path $fixture 'mission.chernarusplus')
    Set-Content (Join-Path $mission 'init.c') 'void main() {}'
    Set-Content (Join-Path $mission 'storage.bin') 'unchanged snapshot'
    Set-Content (Join-Path $server 'DayZServer_x64.exe') 'fixture, never executable'
    $cfg = Join-Path $fixture 'server.cfg'; Set-Content $cfg 'hostname="fixture"; passwordAdmin="never export"; class Missions { class Test { template="dayzOffline.chernarusplus"; }; };'
    $world = Join-Path $fixture 'world.json'; Set-Content $world '{"map":"test","snapshot":"fixture"}'
    $modA = New-Item -ItemType Directory (Join-Path $fixture 'modA/addons') -Force
    $modB = New-Item -ItemType Directory (Join-Path $fixture 'modB/addons') -Force
    Set-Content (Join-Path $modA 'z.pbo') 'z'; Set-Content (Join-Path $modA 'a.pbo') 'a'
    Set-Content (Join-Path $modA 'modperf_bench.pbo') 'benchmark fixture'
    Set-Content (Join-Path $modB 'b.pbo') 'b'
    $profile = Join-Path $fixture 'profile'
    $params = @{ServerRoot=$server.FullName;ProfileDir=$profile;MissionSource=$mission.FullName;ScenarioId='fixture';WorldManifestPath=$world;ServerConfig=$cfg;ModPaths=@($modA.Parent.FullName,$modB.Parent.FullName);HardwareNote='fixture'}
    Assert (Test-Path (Join-Path $PSScriptRoot 'run-baseline.ps1')) 'baseline preparation script is implemented'
    $result = & (Join-Path $PSScriptRoot 'run-baseline.ps1') @params
    Assert ((Get-Content (Join-Path $profile 'server.cfg') -Raw) -match [regex]::Escape((Join-Path $profile 'scenario.chernarusplus').Replace('\','/'))) 'effective config points only at cloned mission'
    Assert ((Get-Content (Join-Path $profile 'server.cfg') -Raw) -match 'bindIP = "127.0.0.1"') 'server binds only to loopback'
    Assert (-not $result.launched) 'default must never launch'
    Assert ($result.arguments.Count -eq 7 -and $result.arguments[-1] -eq '-dologs' -and $result.arguments[-2] -eq '-ip=127.0.0.1') 'launch arguments remain separate, loopback bound and observer mode fixed'
    $config = Get-Content (Join-Path $profile 'modperf-bench/config.json') -Raw | ConvertFrom-Json
    Assert ($config.benches[0] -eq 'BASELINE' -and $config.repeats -eq 3) 'baseline config defaults'
    $manifestFile = Join-Path $profile 'modperf-bench/manifest.json'
    $manifest = Get-Content $manifestFile -Raw | ConvertFrom-Json
    Assert ($config.manifest_sha256 -eq (Get-FileHash $manifestFile -Algorithm SHA256).Hash.ToLowerInvariant()) 'exact manifest hash bound to config'
    Assert ($manifest.mods[0].label -eq 'modA' -and $manifest.mods[1].label -eq 'modB') 'mod order retained'
    Assert ($manifest.mods[0].pbos[0].name -eq 'addons/a.pbo') 'PBO hashes deterministically ordered'
    Assert ((Get-Content $manifestFile -Raw) -notmatch 'never export') 'raw server secrets excluded'
    Assert ((Get-FileHash (Join-Path $mission 'storage.bin')).Hash -eq (Get-FileHash (Join-Path $profile 'scenario.chernarusplus/storage.bin')).Hash) 'scenario snapshot unchanged'
    $refused = $false
    try { & (Join-Path $PSScriptRoot 'run-baseline.ps1') @params | Out-Null } catch { $refused = $_.Exception.Message -match 'fresh|exists' }
    Assert $refused 'existing armed profile refused'
    $hiddenAncestor = New-Item -ItemType Directory (Join-Path $fixture '.hidden-ancestor')
    $hiddenAncestor.Attributes = $hiddenAncestor.Attributes -bor [IO.FileAttributes]::Hidden
    $dryParams = $params.Clone(); $dryParams.ProfileDir = Join-Path $hiddenAncestor.FullName 'explicit-dry-profile'
    $dryResult = & (Join-Path $PSScriptRoot 'run-baseline.ps1') @dryParams -DryRun
    Assert (-not $dryResult.launched) 'explicit DryRun cannot launch'
    $dryManifest = Get-Content $dryResult.manifest_path -Raw | ConvertFrom-Json
    Assert ($manifest.effective_config_sha256 -ne $dryManifest.effective_config_sha256) 'independent clones retain distinct exact effective configs'
    Assert ($manifest.effective_config_comparison_sha256 -match '^[0-9a-f]{64}$' -and $manifest.effective_config_comparison_sha256 -eq $dryManifest.effective_config_comparison_sha256) 'only clone mission path normalizes for config comparison'
    Assert ($manifest.effective_config_normalization -eq 'mission-template-path/1') 'config normalization is versioned'
    $changedConfig = Join-Path $fixture 'changed-server.cfg'
    $effectiveText = Get-Content (Join-Path $profile 'server.cfg') -Raw
    [IO.File]::WriteAllText($changedConfig, ($effectiveText + "`nmaxPlayers = 10;`n"), (New-Object Text.UTF8Encoding $false))
    $changedOutput = Join-Path $fixture 'changed-manifest.json'
    & (Join-Path $PSScriptRoot 'write-run-manifest.ps1') -OutputPath $changedOutput -ServerRoot $server.FullName -ServerConfig $cfg -EffectiveServerConfig $changedConfig -WorldManifestPath $world -MissionSource (Join-Path $profile 'scenario.chernarusplus') -ScenarioId 'fixture' -ModPaths $params.ModPaths | Out-Null
    $changedManifest = Get-Content $changedOutput -Raw | ConvertFrom-Json
    Assert ($changedManifest.effective_config_comparison_sha256 -ne $manifest.effective_config_comparison_sha256) 'other effective runtime options remain part of comparison identity'
    $window = @{frames=100;blocks=2;mean_ms=1.0;elapsed_s=0.1;samples_ms=@(1.0,1.1);samples_kept=2;decimated=$false;timing_source='entry_wall_clock';tail_coverage=$false}
    $artifact = @{schema='modperf-bench/results/2';protocol_version='2.0';manifest_sha256=$config.manifest_sha256;config=$config;clock=@{source='GetTickTime';frame_pairing_valid=$false;observer_qualified=$false};benches=@(@{id='BASELINE';protocol='observed_baseline';verdict='REQUIRES_ANALYSIS';validity_reasons=@();windows=@{baseline=$window}})}
    $resultsFile = Join-Path $fixture 'results.json'
    $artifact | ConvertTo-Json -Depth 12 | Set-Content $resultsFile
    $copied = Join-Path $fixture 'retained-evidence'
    $checked = & (Join-Path $PSScriptRoot 'validate-results.ps1') -ResultsPath $resultsFile -ManifestPath $manifestFile -OutputDir $copied
    Assert ($checked.status -eq 'STRUCTURALLY_VALID' -and -not $checked.qualified) 'valid structure cannot become capacity qualification'
    Assert ((Get-FileHash (Join-Path $copied 'manifest.json')).Hash -eq (Get-FileHash $manifestFile).Hash) 'companion manifest retained byte-for-byte'
    $baselineRow = $artifact.benches[0]
    $b3Row = @{id='B3';protocol='paired_loops';verdict='MEASURED';validity_reasons=@();pair_count=1;pairs=@(@{index=0;order='control_lookup';calls=100;control_ms=0.0;lookup_ms=1.0;delta_ms=1.0})}
    $failedRow = @{id='B1_DUE_CALLLATER';protocol='a_b_a_prime';verdict='INVALID_MULTIPLICITY';validity_reasons=@('NO_FIRES');windows=@{}}
    $artifact.benches = @($baselineRow,$b3Row,$failedRow)
    $artifact | ConvertTo-Json -Depth 12 | Set-Content $resultsFile
    $mixedCopy = Join-Path $fixture 'mixed-evidence'
    $mixedCheck = & (Join-Path $PSScriptRoot 'validate-results.ps1') -ResultsPath $resultsFile -ManifestPath $manifestFile -OutputDir $mixedCopy
    Assert ($mixedCheck.invalid_rows -eq 1 -and -not $mixedCheck.qualified) 'failed dependency rows remain retained and unqualified'
    $retainedMixed = Get-Content (Join-Path $mixedCopy 'results.json') -Raw | ConvertFrom-Json
    Assert ($retainedMixed.benches[2].verdict -eq 'INVALID_MULTIPLICITY' -and $retainedMixed.benches[2].validity_reasons[0] -eq 'NO_FIRES') 'failure verdict and reasons preserved with independent baseline and B3'
    $artifact.benches = @($baselineRow)
    $artifact.manifest_sha256 = 'bad'
    $artifact | ConvertTo-Json -Depth 12 | Set-Content $resultsFile
    $refused = $false
    try { & (Join-Path $PSScriptRoot 'validate-results.ps1') -ResultsPath $resultsFile -ManifestPath $manifestFile | Out-Null } catch { $refused = $_.Exception.Message -match 'digest mismatch' }
    Assert $refused 'manifest mismatch rejected'
    $artifact.manifest_sha256 = $config.manifest_sha256
    $window.samples_ms = @(1.0,-1.0)
    $artifact | ConvertTo-Json -Depth 12 | Set-Content $resultsFile
    $refused = $false
    try { & (Join-Path $PSScriptRoot 'validate-results.ps1') -ResultsPath $resultsFile -ManifestPath $manifestFile | Out-Null } catch { $refused = $_.Exception.Message -match 'positive' }
    Assert $refused 'negative retained sample rejected'
    # A controlled process lookup exercises refusal without launching any executable.
    function Get-Process { [pscustomobject]@{ProcessName='DayZ_x64';Id=123} }
    $params.ProfileDir = Join-Path $fixture 'blocked-profile'
    $refused = $false
    try { & (Join-Path $PSScriptRoot 'run-baseline.ps1') @params -Run | Out-Null } catch { $refused = $_.Exception.Message -match 'running' }
    Assert $refused 'running DayZ client refuses launch before profile mutation'
    Assert (-not (Test-Path $params.ProfileDir)) 'safety refusal creates no armed files'
    Remove-Item Function:Get-Process
    Write-Host 'PASS: baseline preparation, identity, isolation, and launch refusal; no game/server process launched.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'mpb-tools-*') { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
$ErrorActionPreference = 'Stop'
Assert (Test-Path (Join-Path $PSScriptRoot 'validate-results.ps1')) 'results validator is implemented'
