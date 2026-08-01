[CmdletBinding()]
param(
    [Parameter()]
    [ValidatePattern('^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')]
    [string]$Version = '0.1.0',

    [Parameter()]
    [ValidateSet('x64')]
    [string]$Architecture = 'x64',

    [Parameter()]
    [switch]$IncludeMsix
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$packageRoot = Join-Path $repositoryRoot 'out\packages'
$verificationRoot = Join-Path $repositoryRoot 'out\package-verification'

$expected = @(
    "NativeShift-$Version-portable-windows-$Architecture.zip",
    "NativeShift-CLI-$Version-windows-$Architecture.zip",
    "NativeShift-Symbols-$Version-windows-$Architecture.zip",
    "NativeShift-$Version-sbom.spdx.json",
    "NativeShift-$Version-release-manifest.json",
    'SHA256SUMS'
)
if ($IncludeMsix) {
    $expected += "NativeShift-$Version-windows-$Architecture.msix"
}
foreach ($name in $expected) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $name))) {
        throw "Expected package is missing: $name"
    }
}

$checksumPath = Join-Path $packageRoot 'SHA256SUMS'
foreach ($line in Get-Content -LiteralPath $checksumPath) {
    if ($line -notmatch '^([a-f0-9]{64})  (.+)$') {
        throw "Invalid checksum line: $line"
    }
    $file = Join-Path $packageRoot $Matches[2]
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Checksum refers to a missing file: $($Matches[2])"
    }
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Matches[1]) {
        throw "Checksum mismatch for $($Matches[2])"
    }
}

if (Test-Path -LiteralPath $verificationRoot) {
    Remove-Item -LiteralPath $verificationRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $verificationRoot | Out-Null

$archives = Get-ChildItem -LiteralPath $packageRoot -Filter '*.zip' -File
foreach ($archive in $archives) {
    $destination = Join-Path $verificationRoot $archive.BaseName
    Expand-Archive -LiteralPath $archive.FullName -DestinationPath $destination
    if (-not (Get-ChildItem -LiteralPath $destination -File -Recurse)) {
        throw "Archive is empty: $($archive.Name)"
    }
}

$msixRoot = $null
if ($IncludeMsix) {
    $makeAppx = Get-Command makeappx.exe -ErrorAction SilentlyContinue
    if (-not $makeAppx) {
        $makeAppx = Get-ChildItem `
            (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') `
            -Filter makeappx.exe -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object FullName -Match '\\x64\\makeappx\.exe$' |
            Sort-Object FullName -Descending |
            Select-Object -First 1
    }
    if (-not $makeAppx) {
        throw 'makeappx.exe was not found.'
    }
    $makeAppxPath = if (
        $makeAppx -is [Management.Automation.CommandInfo]
    ) {
        $makeAppx.Source
    } else {
        $makeAppx.FullName
    }
    $msixRoot = Join-Path $verificationRoot 'NativeShift-MSIX'
    & $makeAppxPath unpack `
        /p (Join-Path $packageRoot "NativeShift-$Version-windows-$Architecture.msix") `
        /d $msixRoot /o | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'The MSIX could not be unpacked.'
    }

    [xml]$appxManifest = Get-Content -LiteralPath (Join-Path $msixRoot 'AppxManifest.xml') -Raw
    $identity = $appxManifest.Package.Identity
    $semanticVersion = ($Version -split '-', 2)[0]
    if ($identity.ProcessorArchitecture -ne $Architecture) {
        throw "MSIX architecture is $($identity.ProcessorArchitecture), expected $Architecture."
    }
    if ($identity.Version -notlike "$semanticVersion.*") {
        throw "MSIX version is $($identity.Version), expected $semanticVersion.x."
    }
}

$payloadRoots = Get-ChildItem -LiteralPath $verificationRoot -Directory |
    Where-Object Name -notmatch 'Symbols'
foreach ($root in $payloadRoots) {
    $files = Get-ChildItem -LiteralPath $root.FullName -File -Recurse
    $forbidden = $files | Where-Object {
        $_.Name -match '(?i)((?:^|[-_0-9])d\.dll$|^zd\.dll$|\.pdb$|\.cpp$|\.h$|\.pfx$|\.pem$|\.key$|\.env$)' -or
        ($_.Name -match '(?i)(test|fixture)' -and
            $_.Extension -in @('.exe', '.dll', '.lib', '.pdb')) -or
        $_.FullName -match '(?i)\\(tests?|fixtures?|src|vcpkg_installed)\\'
    }
    if ($forbidden) {
        throw "Forbidden payload file: $($forbidden[0].FullName)"
    }
    if (-not ($files.Name -contains 'LICENSE') -or
        -not ($files.Name -contains 'THIRD_PARTY_NOTICES.md')) {
        throw "License material is missing from $($root.Name)."
    }
}

$portable = Join-Path $verificationRoot "NativeShift-$Version-portable-windows-$Architecture"
$cli = Join-Path $verificationRoot "NativeShift-CLI-$Version-windows-$Architecture"
$requiredPayload = @(
    (Join-Path $portable 'NativeShift.exe'),
    (Join-Path $portable 'NativeShift.GuiBridge.dll'),
    (Join-Path $portable 'App.xbf'),
    (Join-Path $portable 'MainWindow.xbf'),
    (Join-Path $cli 'nativeshift-cli.exe')
)
if ($IncludeMsix) {
    $requiredPayload += @(
        (Join-Path $msixRoot 'NativeShift.exe'),
        (Join-Path $msixRoot 'NativeShift.GuiBridge.dll'),
        (Join-Path $msixRoot 'AppxManifest.xml')
    )
}
foreach ($required in $requiredPayload) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required executable or bridge is missing: $required"
    }
}

$versionOutput = & (Join-Path $cli 'nativeshift-cli.exe') --version
if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch [regex]::Escape($Version)) {
    throw 'Packaged CLI did not report the expected version.'
}

$portableProcess = Start-Process `
    -FilePath (Join-Path $portable 'NativeShift.exe') `
    -PassThru `
    -WindowStyle Hidden
try {
    Start-Sleep -Seconds 3
    if ($portableProcess.HasExited) {
        throw "Portable application exited during startup with code $($portableProcess.ExitCode)."
    }
} finally {
    if (-not $portableProcess.HasExited) {
        Stop-Process -Id $portableProcess.Id -Force
    }
}

foreach ($jsonName in @(
    "NativeShift-$Version-sbom.spdx.json",
    "NativeShift-$Version-release-manifest.json"
)) {
    $document = Get-Content -LiteralPath (Join-Path $packageRoot $jsonName) -Raw |
        ConvertFrom-Json
    if (-not $document) {
        throw "JSON document is empty: $jsonName"
    }
}

$sbom = Get-Content -LiteralPath (
    Join-Path $packageRoot "NativeShift-$Version-sbom.spdx.json"
) -Raw | ConvertFrom-Json
$sbomPackages = @($sbom.packages | ForEach-Object name)
foreach ($requiredPackage in @(
    'NativeShift',
    'ffmpeg',
    'libpng',
    'libwebp',
    'Microsoft.WindowsAppSDK'
)) {
    if ($requiredPackage -notin $sbomPackages) {
        throw "SBOM is missing required package: $requiredPackage"
    }
}

$secretPatterns = @(
    '-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    '(?i)(?:password|client_secret|access_token)\s*[:=]\s*["''][^"'']{8,}',
    'gh[pousr]_[A-Za-z0-9_]{20,}'
)
foreach ($root in $payloadRoots) {
    Get-ChildItem -LiteralPath $root.FullName -File -Recurse |
        Where-Object { $_.Length -lt 5MB -and $_.Extension -notin @('.exe', '.dll', '.png', '.pri') } |
        ForEach-Object {
            $content = Get-Content -LiteralPath $_.FullName -Raw -ErrorAction SilentlyContinue
            foreach ($pattern in $secretPatterns) {
                if ($content -match $pattern) {
                    throw "Secret-like content found in $($_.FullName)."
                }
            }
        }
}

Write-Host "Verified NativeShift $Version package candidates."
