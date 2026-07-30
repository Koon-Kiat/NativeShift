[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$toolRoot = Join-Path $repositoryRoot 'out\workflow-tools'
New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null

function Get-VerifiedTool {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Url,
        [Parameter(Mandatory)][string]$Sha256,
        [Parameter(Mandatory)][string]$ArchiveMember
    )
    $archive = Join-Path $toolRoot "$Name.zip"
    $destination = Join-Path $toolRoot $Name
    if ((Test-Path -LiteralPath $archive) -and
        (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Sha256) {
        Remove-Item -LiteralPath $archive -Force
    }
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri $Url -OutFile $archive
    }
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) {
        throw "$Name checksum mismatch."
    }
    if (-not (Test-Path -LiteralPath $destination)) {
        $extract = Join-Path $toolRoot "$Name-extract"
        if (Test-Path -LiteralPath $extract) {
            Remove-Item -LiteralPath $extract -Recurse -Force
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $extract
        $member = Get-ChildItem -LiteralPath $extract -File -Recurse |
            Where-Object Name -eq $ArchiveMember |
            Select-Object -First 1
        if (-not $member) {
            throw "$ArchiveMember was not found in $Name."
        }
        Copy-Item -LiteralPath $member.FullName -Destination $destination
    }
    return $destination
}

$actionlint = Get-VerifiedTool `
    -Name 'actionlint.exe' `
    -Url 'https://github.com/rhysd/actionlint/releases/download/v1.7.7/actionlint_1.7.7_windows_amd64.zip' `
    -Sha256 '7f12f1801bca3d480d67aaf7774f4c2a6359a3ca8eebe382c95c10c9704aa731' `
    -ArchiveMember 'actionlint.exe'

$zizmorArchive = Join-Path $toolRoot 'zizmor.zip'
if (-not (Test-Path -LiteralPath $zizmorArchive)) {
    Invoke-WebRequest `
        -Uri 'https://github.com/zizmorcore/zizmor/releases/download/v1.11.0/zizmor-x86_64-pc-windows-msvc.zip' `
        -OutFile $zizmorArchive
}
$zizmorHash = (Get-FileHash -LiteralPath $zizmorArchive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($zizmorHash -ne '35e038bdbde6fcfdf947c947c7c3fc83c5043e0ded0e5b0d59c30c8eda97fd3a') {
    throw 'zizmor checksum mismatch.'
}
$zizmor = Join-Path $toolRoot 'zizmor.exe'
if (-not (Test-Path -LiteralPath $zizmor)) {
    Expand-Archive -LiteralPath $zizmorArchive -DestinationPath $toolRoot -Force
}

Push-Location $repositoryRoot
try {
    & $actionlint
    if ($LASTEXITCODE -ne 0) {
        throw 'actionlint reported workflow errors.'
    }
    & $zizmor .github/workflows
    if ($LASTEXITCODE -ne 0) {
        throw 'zizmor reported workflow security findings.'
    }
} finally {
    Pop-Location
}

Write-Host 'Workflow syntax and security checks passed.'
