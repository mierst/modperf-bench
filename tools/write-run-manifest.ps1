[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputPath,
    [Parameter(Mandatory)][string]$ServerRoot,
    [Parameter(Mandatory)][string]$ServerConfig,
    [Parameter(Mandatory)][string]$WorldManifestPath,
    [Parameter(Mandatory)][string]$MissionSource,
    [Parameter(Mandatory)][string]$ScenarioId,
    [string[]]$ModPaths = @(), [string]$HardwareNote = '',
    [ValidateRange(0,10000)][int]$FpsLimit = 0,
    [string]$EffectiveServerConfig = '',
    [string]$EngineExecutable = 'DayZServer_x64.exe'
)
$ErrorActionPreference = 'Stop'
function Hash-File([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Hash-Text([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
}
function Tree-Identity([string]$Root, [string]$Filter = '*') {
    $rootPath = (Resolve-Path -LiteralPath $Root).Path
    $files = @(Get-ChildItem -LiteralPath $rootPath -File -Recurse -Force -Filter $Filter | Sort-Object FullName)
    $entries = @($files | ForEach-Object { [ordered]@{name=$_.FullName.Substring($rootPath.Length).TrimStart('\','/').Replace('\','/');sha256=(Hash-File $_.FullName)} })
    [pscustomobject]@{sha256=(Hash-Text (($entries | ForEach-Object { $_.name + ':' + $_.sha256 }) -join "`n"));files=$entries}
}
foreach ($required in @($ServerConfig,$WorldManifestPath)) { if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing identity input: $required" } }
$engine = Join-Path (Resolve-Path -LiteralPath $ServerRoot).Path $EngineExecutable
if (-not (Test-Path -LiteralPath $engine -PathType Leaf)) { throw 'Dedicated server executable missing.' }
$mods = @($ModPaths | ForEach-Object {
    $root = (Resolve-Path -LiteralPath $_).Path
    $identity = Tree-Identity $root '*.pbo'
    if ($identity.files.Count -eq 0) { throw "No PBOs under mod: $root" }
    [ordered]@{label=(Split-Path $root -Leaf);path=(Split-Path $root -Leaf);sha256=$identity.sha256;pbos=$identity.files}
})
$scenario = Tree-Identity $MissionSource
$effectiveComparison = $null
$effectiveNormalization = $null
if ($EffectiveServerConfig) {
    $effectiveText = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $EffectiveServerConfig).Path).Replace("`r`n", "`n")
    $missionValuePattern = '(\btemplate\s*=\s*")[^"]*("\s*;)'
    if ([regex]::Matches($effectiveText, $missionValuePattern).Count -ne 1) { throw 'Effective config normalization requires exactly one mission template.' }
    $normalizedText = [regex]::Replace($effectiveText, $missionValuePattern, '${1}__MODPERF_SCENARIO__${2}')
    $effectiveComparison = Hash-Text $normalizedText
    $effectiveNormalization = 'mission-template-path/1'
}
$repo = Split-Path $PSScriptRoot -Parent
$source = $null
$dirty = $null
$sourceReason = 'Git metadata unavailable.'
$previousErrorAction = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $gitSource = & git -c "safe.directory=$repo" -C $repo rev-parse HEAD 2>$null
        if ($LASTEXITCODE -eq 0) { $source = [string]$gitSource; $sourceReason = $null }
        $gitStatus = @(& git -c "safe.directory=$repo" -C $repo status --porcelain 2>$null)
        if ($LASTEXITCODE -eq 0) { $dirty = $gitStatus.Count -gt 0 }
    }
} catch { $sourceReason = 'Git metadata lookup failed.' } finally { $ErrorActionPreference = $previousErrorAction }
$cpu = $null
try { $cpu = @(Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors) } catch { }
$manifest = [ordered]@{
    schema='modperf-bench/manifest/1';protocol_version='2.0';scenario_id=$ScenarioId
    config_sha256=(Hash-File $ServerConfig);effective_config_sha256=$(if ($EffectiveServerConfig) {Hash-File $EffectiveServerConfig} else {$null});world_sha256=(Hash-File $WorldManifestPath)
    effective_config_comparison_sha256=$effectiveComparison;effective_config_normalization=$effectiveNormalization
    scenario_snapshot_sha256=$scenario.sha256;scenario_file_count=$scenario.files.Count
    engine_sha256=(Hash-File $engine);host_id=(Hash-Text ([Environment]::MachineName))
    platform=[Environment]::OSVersion.Platform.ToString();os_version=[Environment]::OSVersion.VersionString
    observer_mode='recording';fps_limit=$(if ($FpsLimit -gt 0) {$FpsLimit} else {$null})
    operating_regime=$(if ($FpsLimit -gt 0) {'capped'} else {'uncapped_requested'})
    mods=$mods;source_commit=$source;source_dirty=$dirty;source_unknown_reason=$sourceReason
    hardware_note=$HardwareNote
    environment=[ordered]@{cpu=$cpu;cpu_allocation=$null;power_thermal=$null;memory_context=$null;storage_context=$null;virtualization=$null;contention=$null;unknown_reason='Not measured by preparation tooling; operator must qualify allocation and runtime conditions.'}
    scenario_notes='Identity hashes describe the supplied starting snapshot; activation, entity counts, save cycles and workload completion require runtime evidence.'
}
if (Test-Path -LiteralPath $OutputPath) { throw 'Manifest output already exists; use a fresh profile.' }
$parent = Split-Path ([IO.Path]::GetFullPath($OutputPath)) -Parent
New-Item -ItemType Directory -Force -Path $parent | Out-Null
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputPath), ($manifest | ConvertTo-Json -Depth 12), (New-Object Text.UTF8Encoding $false))
[pscustomobject]@{path=[IO.Path]::GetFullPath($OutputPath);sha256=(Hash-File $OutputPath)}
