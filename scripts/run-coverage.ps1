[CmdletBinding()]
param(
    [Parameter()]
    [ValidateRange(1, 100)]
    [int]$MinimumLinePercent = 65
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildRoot = Join-Path $repositoryRoot 'out\build\nativeshift-debug'
$coverageRoot = Join-Path $repositoryRoot 'out\coverage'
$cobertura = Join-Path $coverageRoot 'cobertura.xml'
$html = Join-Path $coverageRoot 'html'
New-Item -ItemType Directory -Path $coverageRoot -Force | Out-Null

$coverageTool = Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue
if (-not $coverageTool) {
    throw 'OpenCppCoverage.exe was not found.'
}

& $coverageTool.Source `
    "--sources=$(Join-Path $repositoryRoot 'src\NativeShift.Core')" `
    "--sources=$(Join-Path $repositoryRoot 'src\NativeShift.Image')" `
    "--sources=$(Join-Path $repositoryRoot 'src\NativeShift.Media')" `
    "--sources=$(Join-Path $repositoryRoot 'src\NativeShift.Platform.Windows')" `
    "--excluded_sources=$(Join-Path $repositoryRoot 'tests')" `
    "--excluded_sources=$(Join-Path $buildRoot 'vcpkg_installed')" `
    "--export_type=cobertura:$cobertura" `
    "--export_type=html:$html" `
    '--' `
    'ctest.exe' `
    '--test-dir' $buildRoot `
    '--timeout' '180' `
    '--output-on-failure'
if ($LASTEXITCODE -ne 0) {
    throw "Coverage test execution failed with exit code $LASTEXITCODE."
}

[xml]$report = Get-Content -LiteralPath $cobertura -Raw
$linePercent = [math]::Round(
    [double]::Parse(
        $report.coverage.'line-rate',
        [Globalization.CultureInfo]::InvariantCulture
    ) * 100,
    2
)
Write-Host "Project-owned line coverage: $linePercent%"
if ($linePercent -lt $MinimumLinePercent) {
    throw "Coverage $linePercent% is below the $MinimumLinePercent% gate."
}
