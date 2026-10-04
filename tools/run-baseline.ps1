[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ServerRoot,
    [Parameter(Mandatory)][string]$ProfileDir,
    [Parameter(Mandatory)][string]$MissionSource,
    [Parameter(Mandatory)][string]$ScenarioId,
    [Parameter(Mandatory)][string]$WorldManifestPath,
    [Parameter(Mandatory)][string]$ServerConfig,
    [Parameter(Mandatory)][string[]]$ModPaths,
    [string]$HardwareNote = '', [ValidateRange(0,10000)][int]$FpsLimit = 0,
    [ValidateRange(1,86400)][int]$DurationSeconds = 300,
    [ValidateRange(1,600)][int]$WarmupSeconds = 60,
    [ValidateRange(1,600)][int]$WindowSeconds = 60,
    [ValidateRange(1,20)][int]$Repeats = 3,
    [ValidateRange(0.001,10)][double]$BlockSeconds = 0.01,
    [ValidateRange(1,65535)][int]$Port = 2402,
    [switch]$Run, [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
function Inside([string]$Candidate,[string]$Root) {
    $prefix = $Root.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    $Candidate.Equals($Root,[StringComparison]::OrdinalIgnoreCase) -or $Candidate.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)
}
if ($Run -and $DryRun) { throw 'Choose -Run or -DryRun, not both.' }
if ($Run) {
    $active = @(Get-Process -ErrorAction Stop | Where-Object { $_.ProcessName -match '^DayZ' })
    if ($active.Count -gt 0) { throw 'DayZ client or server is running; launch refused. No processes are stopped.' }
}
$server = (Resolve-Path -LiteralPath $ServerRoot).Path
if (-not (Test-Path -LiteralPath $MissionSource -PathType Container)) { throw 'MissionSource must be an offline mission directory.' }
$mission = (Resolve-Path -LiteralPath $MissionSource).Path
if ([string]::IsNullOrEmpty([IO.Path]::GetExtension($mission))) { throw 'MissionSource name must retain its terrain suffix, for example .chernarusplus.' }
$profile = [IO.Path]::GetFullPath($ProfileDir)
$ancestor = $profile
while ($ancestor) {
    if (Test-Path -LiteralPath $ancestor) {
        if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Profile ancestors must not be reparse points.' }
    }
    $ancestor = Split-Path $ancestor -Parent
}
if ((Inside $profile $server) -or (Inside $server $profile) -or (Inside $profile $mission) -or (Inside $mission $profile)) { throw 'Profile must be isolated from server installation and mission source.' }
if (Test-Path -LiteralPath $profile) { throw 'Profile already exists; provide a fresh dedicated profile directory.' }
foreach ($path in @($server,$mission)) {
    if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse-point roots are not supported.' }
}
if (@(Get-ChildItem -LiteralPath $mission -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count -gt 0) { throw 'Mission snapshot contains reparse points; refusing an ambiguous clone.' }
if ($ModPaths.Count -eq 0) { throw 'Supply the benchmark mod and ordered dependencies through -ModPaths.' }
$mods = @($ModPaths | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
$benchmarkPbos = @($mods | ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -File -Filter 'modperf_bench.pbo' } | Where-Object { $_.Length -gt 0 })
if ($benchmarkPbos.Count -ne 1) { throw 'Ordered ModPaths must include exactly one nonempty modperf_bench.pbo.' }
if ($BlockSeconds -gt ($WindowSeconds / 10.0)) { throw 'BlockSeconds must not exceed WindowSeconds / 10.' }
foreach ($path in @($profile,$server,$mission) + $mods) { if ($path -match '[";\r\n]') { throw 'Paths containing quotes, semicolons or newlines are not supported.' } }
if ($DurationSeconds -lt ($WarmupSeconds + $Repeats * ($WindowSeconds + 2))) { throw 'DurationSeconds must cover warmup, every window and transition allowance.' }
New-Item -ItemType Directory -Path $profile | Out-Null
$clone = Join-Path $profile ('scenario' + [IO.Path]::GetExtension($mission))
$sourceFiles = @(Get-ChildItem -LiteralPath $mission -Recurse -File -Force | ForEach-Object {
    [pscustomobject]@{Relative=$_.FullName.Substring($mission.Length).TrimStart('\','/');Hash=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
Copy-Item -LiteralPath $mission -Destination $clone -Recurse
foreach ($file in $sourceFiles) {
    $copy = Join-Path $clone $file.Relative
    if (-not (Test-Path -LiteralPath $copy -PathType Leaf) -or (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -ne $file.Hash) { throw 'Snapshot clone differs from the captured source; do not run this profile.' }
}
if (@(Get-ChildItem -LiteralPath $clone -File -Recurse -Force).Count -ne $sourceFiles.Count) { throw 'Snapshot file count changed during cloning.' }
$localConfig = Join-Path $profile 'server.cfg'
$configText = Get-Content -LiteralPath $ServerConfig -Raw
$templatePattern = '(?m)\btemplate\s*=\s*"[^"]*"\s*;'
if ([regex]::Matches($configText,$templatePattern).Count -ne 1) { throw 'Server config must contain exactly one mission template; refusing an ambiguous mission selection.' }
$configText = [regex]::Replace($configText,$templatePattern,('template = "' + $clone.Replace('\','/') + '";'))
$configText = [regex]::Replace($configText,'(?m)\bbindIP\s*=\s*"[^"]*"\s*;','')
$configText += "`nbindIP = `"127.0.0.1`";`n"
[IO.File]::WriteAllText($localConfig, $configText, (New-Object Text.UTF8Encoding $false))
$evidence = Join-Path $profile 'modperf-bench'
$manifest = & (Join-Path $PSScriptRoot 'write-run-manifest.ps1') -OutputPath (Join-Path $evidence 'manifest.json') -ServerRoot $server -ServerConfig $ServerConfig -EffectiveServerConfig $localConfig -WorldManifestPath $WorldManifestPath -MissionSource $clone -ScenarioId $ScenarioId -ModPaths $mods -HardwareNote $HardwareNote -FpsLimit $FpsLimit
$config = [ordered]@{note='Operational observed baseline; no population qualification';hardware_note=$HardwareNote;scenario_id=$ScenarioId;manifest_sha256=$manifest.sha256;benches=@('BASELINE');settle_seconds=$WarmupSeconds;window_seconds=$WindowSeconds;block_seconds=$BlockSeconds;repeats=$Repeats;transition_seconds=2}
[IO.File]::WriteAllText((Join-Path $evidence 'config.json'), ($config | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding $false))
$arguments = @(
    ('-config="' + $localConfig + '"'),
    ('-profiles="' + $profile + '"'),
    ('-mission="' + $clone + '"'),
    ('-serverMod="' + ($mods -join ';') + '"'),
    ('-port=' + $Port),
    '-ip=127.0.0.1',
    '-dologs'
)
if ($FpsLimit -gt 0) { $arguments += '-limitFPS=' + $FpsLimit }
$processId = $null
if ($Run) {
    # Recheck immediately before launch; a process appearing during preparation must block.
    if (@(Get-Process -ErrorAction Stop | Where-Object { $_.ProcessName -match '^DayZ' }).Count -gt 0) { throw 'DayZ is running; launch refused after preparation.' }
    $proc = Start-Process -FilePath (Join-Path $server 'DayZServer_x64.exe') -ArgumentList $arguments -WorkingDirectory $server -WindowStyle Hidden -PassThru
    $processId = $proc.Id
}
[pscustomobject]@{launched=[bool]$Run;process_id=$processId;profile_dir=$profile;manifest_path=$manifest.path;manifest_sha256=$manifest.sha256;arguments=$arguments;observation_budget_seconds=$DurationSeconds;shutdown='Manual graceful server shutdown required; this tool never kills a process.'}
