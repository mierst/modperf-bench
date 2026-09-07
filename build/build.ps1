# =============================================================================
# build.ps1 -- package @modperf-bench.
#
#   src/modperf_bench  ->  build/@modperf-bench/addons/modperf_bench.pbo
#
# Requires Mikero DePboTools (MakePbo.exe). No signing: this is a server-side
# mod, and DayZ only verifies signatures for mods clients load. v1 ships
# unsigned on purpose.
#
# WHAT A GREEN BUILD DOES AND DOES NOT PROVE
#   MakePbo packages Enforce Script that does not compile. It checks nothing
#   about the script beyond its existence. The ONLY compile gate is booting a
#   DayZ server with the mod and watching for "Player connect enabled" in the
#   RPT. Treat a successful build as "packaged", never as "correct".
#
# USAGE
#   powershell -File build\build.ps1
#   powershell -File build\build.ps1 -InstallTo "C:\path\to\DayZServer"
#
#   -InstallTo copies the finished @modperf-bench folder next to a server
#   executable, which is where a server expects to find a -serverMod folder.
# =============================================================================

[CmdletBinding()]
param(
    [string]$InstallTo = "",
    [switch]$KeepStaging
)

$ErrorActionPreference = "Stop"

$BuildDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $BuildDir "..")).Path
$SourceDir = Join-Path $RepoRoot "src\modperf_bench"

$AddonName = "modperf_bench"       # PBO name and $PBOPREFIX$
$ModFolderName = "@modperf-bench"  # -serverMod folder name

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
$MikeroBin = "C:\Program Files (x86)\Mikero\DePboTools\bin"
if (-not (Test-Path $MikeroBin)) {
    $MikeroBin = "C:\Program Files\Mikero\DePboTools\bin"
}
if (-not (Test-Path $MikeroBin)) {
    throw "Mikero DePboTools not found. Install DePboTools and re-run."
}
$MakePbo = Join-Path $MikeroBin "MakePbo.exe"
if (-not (Test-Path $MakePbo)) {
    throw "MakePbo.exe not found at $MakePbo"
}

if (-not (Test-Path (Join-Path $SourceDir "config.cpp"))) {
    throw "config.cpp not found under $SourceDir"
}

Write-Host "[build] MakePbo   : $MakePbo"
Write-Host "[build] source    : $SourceDir"

# -----------------------------------------------------------------------------
# Stage
#
# MakePbo is pointed at a staging copy rather than at src/ directly, so the
# generated $PBOPREFIX$ never lands in the working tree and the packed bytes
# are exactly what was staged -- nothing else in src/ can leak into the PBO.
# -----------------------------------------------------------------------------
$StageRoot = Join-Path $BuildDir "staging"
$StageDir = Join-Path $StageRoot $AddonName
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

Copy-Item -Force (Join-Path $SourceDir "config.cpp") (Join-Path $StageDir "config.cpp")
Copy-Item -Recurse -Force (Join-Path $SourceDir "scripts") (Join-Path $StageDir "scripts")

# $PBOPREFIX$ maps the PBO contents into the engine's virtual filesystem. It
# must match CfgMods.dir and every files[] entry in config.cpp, or the engine
# loads the mod and finds no scripts -- which looks exactly like a mod that
# does nothing.
$PrefixFile = Join-Path $StageDir '$PBOPREFIX$'
Set-Content -Path $PrefixFile -Value $AddonName -Encoding ASCII -NoNewline

# Enforce Script has repeatedly tripped over non-ASCII bytes in source. Catch
# it here, where the message can say which file, rather than in the engine.
$badFiles = @()
Get-ChildItem -Recurse -File $StageDir | ForEach-Object {
    $bytes = [System.IO.File]::ReadAllBytes($_.FullName)
    foreach ($b in $bytes) {
        if ($b -gt 127) { $badFiles += $_.FullName; break }
    }
}
if ($badFiles.Count -gt 0) {
    throw "Non-ASCII bytes found in staged source (Enforce tokenizer hazard):`n" + ($badFiles -join "`n")
}

Write-Host "[build] staged    : $StageDir"

# -----------------------------------------------------------------------------
# Pack
# -----------------------------------------------------------------------------
$ModFolder = Join-Path $BuildDir $ModFolderName
$AddonsDir = Join-Path $ModFolder "addons"
if (Test-Path $ModFolder) { Remove-Item -Recurse -Force $ModFolder }
New-Item -ItemType Directory -Force -Path $AddonsDir | Out-Null

$PboPath = Join-Path $AddonsDir ($AddonName + ".pbo")

$stdOut = Join-Path $StageRoot "makepbo.out.log"
$stdErr = Join-Path $StageRoot "makepbo.err.log"
$proc = Start-Process -FilePath $MakePbo `
    -ArgumentList @("-P", ("`"" + $StageDir + "`""), ("`"" + $PboPath + "`"")) `
    -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $stdOut -RedirectStandardError $stdErr

if (Test-Path $stdOut) { Get-Content $stdOut | ForEach-Object { Write-Host "[makepbo] $_" } }
if ((Test-Path $stdErr) -and (Get-Item $stdErr).Length -gt 0) {
    Get-Content $stdErr | ForEach-Object { Write-Warning "[makepbo] $_" }
}

if (-not (Test-Path $PboPath) -or (Get-Item $PboPath).Length -eq 0) {
    throw "MakePbo exited $($proc.ExitCode) and produced no usable PBO at $PboPath"
}

Write-Host ("[build] packed    : {0} ({1:N0} bytes, MakePbo exit {2})" -f $PboPath, (Get-Item $PboPath).Length, $proc.ExitCode)

if (-not $KeepStaging) {
    Remove-Item -Recurse -Force $StageDir
    Remove-Item -Force $stdOut, $stdErr -ErrorAction SilentlyContinue
    if ((Get-ChildItem $StageRoot -Force | Measure-Object).Count -eq 0) {
        Remove-Item -Force $StageRoot
    }
}

# -----------------------------------------------------------------------------
# Optional install next to a server executable
# -----------------------------------------------------------------------------
if ($InstallTo -ne "") {
    if (-not (Test-Path $InstallTo)) {
        throw "-InstallTo path does not exist: $InstallTo"
    }
    $dest = Join-Path $InstallTo $ModFolderName
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Copy-Item -Recurse $ModFolder $dest
    Write-Host "[build] installed : $dest"
}

Write-Host "[build] OK. Remember: a green build is 'packaged', not 'compiles'."
Write-Host "[build] Boot a server with -serverMod=$ModFolderName to compile-verify."
