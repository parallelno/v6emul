<#
.SYNOPSIS
    Configure, build, and (optionally) test v6emul using the project-local .venv.

.DESCRIPTION
    Bootstraps the project virtual environment (.venv) with `uv` when available
    (falling back to `python -m venv`), then runs the CMake preset workflow. The
    environment's Python is placed on PATH so CMake and the ASM unit test runner
    pick it up automatically.

.PARAMETER Preset
    CMake preset to use: debug, release, or ci. Defaults to release.

.PARAMETER Test
    Run the CTest suite after building.

.PARAMETER Clean
    Remove the preset's build directory before configuring.

.EXAMPLE
    ./scripts/build.ps1
    ./scripts/build.ps1 -Preset ci -Test
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'ci')]
    [string]$Preset = 'release',
    [switch]$Test,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$venvDir = Join-Path $root '.venv'
$venvPython = Join-Path $venvDir 'Scripts\python.exe'

# ── Ensure the project virtual environment exists ──────────────────────
if (-not (Test-Path $venvPython)) {
    Write-Host 'Creating project virtual environment (.venv)...'
    if (Get-Command uv -ErrorAction SilentlyContinue) {
        uv venv $venvDir
    } elseif (Get-Command python -ErrorAction SilentlyContinue) {
        python -m venv $venvDir
    } else {
        throw 'Cannot create .venv: install uv (https://docs.astral.sh/uv/) or Python 3.8+ first.'
    }
}

# Make the venv interpreter the default `python` for the whole build.
$env:VIRTUAL_ENV = $venvDir
$env:PATH = (Split-Path -Parent $venvPython) + [IO.Path]::PathSeparator + $env:PATH
Write-Host "Using Python: $(& $venvPython --version 2>&1)"

# ── Optional clean ─────────────────────────────────────────────────────
$buildDir = Join-Path $root "build/$Preset"
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir..."
    Remove-Item -Recurse -Force $buildDir
}

# ── Configure & build ──────────────────────────────────────────────────
cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ── Test ───────────────────────────────────────────────────────────────
if ($Test) {
    $config = if ($Preset -eq 'debug') { 'Debug' } else { 'Release' }
    ctest --test-dir $buildDir --build-config $config --output-on-failure
    exit $LASTEXITCODE
}
