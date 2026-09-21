<#
.SYNOPSIS
    Builds, tests and validates Link Failover Fabric from a fresh clone.

.DESCRIPTION
    Configures and builds the Release and Debug configurations, runs every test
    suite, runs the completed-work benchmark, and proves that the installed
    package is consumable by an independent project outside the source tree.

    The script is location independent: every path is derived from the script's
    own directory. It refuses to run if a required tool is missing rather than
    silently skipping a stage.

.PARAMETER Configuration
    Restrict the run to a subset: "all" (default), "release", "debug" or "quick".

.PARAMETER SkipBenchmark
    Skip the benchmark stage.

.EXAMPLE
    pwsh -File scripts/verify.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'release', 'debug', 'quick')]
    [string] $Configuration = 'all',
    [switch] $SkipBenchmark
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$results = [System.Collections.Generic.List[object]]::new()

function Invoke-Stage {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [scriptblock] $Action,
        [switch] $AllowSkip
    )
    Write-Host ""
    Write-Host "==== $Name ====" -ForegroundColor Cyan
    $started = Get-Date
    $status = 'FAIL'
    $detail = ''
    try {
        $detail = & $Action
        if ($null -eq $detail) { $detail = '' }
        $status = 'PASS'
    } catch {
        $detail = $_.Exception.Message
        if ($AllowSkip -and $detail -like 'SKIP:*') { $status = 'SKIP' }
    }
    $elapsed = (Get-Date) - $started
    $results.Add([pscustomobject]@{
        Stage   = $Name
        Status  = $status
        Seconds = [math]::Round($elapsed.TotalSeconds, 1)
        Detail  = ($detail | Out-String).Trim()
    })
    $colour = switch ($status) { 'PASS' { 'Green' } 'SKIP' { 'Yellow' } default { 'Red' } }
    Write-Host "$Name : $status" -ForegroundColor $colour
}

function Assert-Executable {
    param([string] $Path)
    if (-not (Test-Path $Path)) { throw "missing executable: $Path" }
    return $Path
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'cmake was not found on PATH; run this script from a developer shell'
}

$releaseDir = Join-Path $root 'build'
$debugDir = Join-Path $root 'build-debug'

if ($Configuration -in @('all', 'release', 'quick')) {
    Invoke-Stage 'configure-release' {
        & cmake -S $root -B $releaseDir -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
        'configured'
    }
    Invoke-Stage 'build-release' {
        & cmake --build $releaseDir --parallel 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'release build failed' }
        'built'
    }
}

if ($Configuration -in @('all', 'debug')) {
    Invoke-Stage 'configure-debug' {
        & cmake -S $root -B $debugDir -DCMAKE_BUILD_TYPE=Debug 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
        'configured'
    }
    Invoke-Stage 'build-debug' {
        & cmake --build $debugDir --parallel 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'debug build failed' }
        'built'
    }
}

$suites = @('unit', 'protocol', 'persistence', 'integration', 'property', 'adversarial',
            'concurrency', 'multiprocess', 'restart', 'scale', 'consumer')
if ($Configuration -eq 'quick') {
    $suites = @('unit', 'protocol', 'persistence', 'integration', 'adversarial', 'concurrency')
}

if ($Configuration -in @('all', 'release', 'quick')) {
    $tests = Assert-Executable (Join-Path $releaseDir 'lff-tests.exe')
    foreach ($suite in $suites) {
        Invoke-Stage "tests-$suite" {
            $output = & $tests --suite $suite 2>&1
            $output | Write-Host
            if ($LASTEXITCODE -ne 0) { throw "suite $suite failed" }
            ($output | Select-String 'summary').ToString()
        }
    }
}

if ($Configuration -eq 'all') {
    $debugTests = Assert-Executable (Join-Path $debugDir 'lff-tests.exe')
    Invoke-Stage 'tests-debug' {
        $output = & $debugTests 2>&1
        $output | Select-String 'FAIL|summary' | Write-Host
        if ($LASTEXITCODE -ne 0) { throw 'the debug configuration failed its suites' }
        ($output | Select-String 'summary').ToString()
    }
    $example = Assert-Executable (Join-Path $releaseDir 'lff-example-minimal.exe')
    Invoke-Stage 'example' {
        $output = & $example 2>&1
        if ($LASTEXITCODE -ne 0) { throw 'the example failed' }
        ($output | Select-String 'failover').ToString()
    }
    if (-not $SkipBenchmark) {
        $bench = Assert-Executable (Join-Path $releaseDir 'lff-bench.exe')
        Invoke-Stage 'benchmark' {
            $output = & $bench 1 2>&1
            $output | Write-Host
            if ($LASTEXITCODE -ne 0) { throw 'the benchmark failed' }
            'completed'
        }
    }
}

Write-Host ""
Write-Host "==== summary ====" -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-String | Write-Host
$failed = @($results | Where-Object { $_.Status -eq 'FAIL' })
if ($failed.Count -gt 0) {
    Write-Host "$($failed.Count) stage(s) failed" -ForegroundColor Red
    exit 1
}
Write-Host "all stages passed" -ForegroundColor Green
exit 0
