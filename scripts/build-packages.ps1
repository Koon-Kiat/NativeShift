[CmdletBinding()]
param(
    [Parameter()]
    [ValidatePattern('^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')]
    [string]$Version = '0.2.0',

    [Parameter()]
    [ValidateSet('x64')]
    [string]$Architecture = 'x64',

    [Parameter()]
    [switch]$SkipBuild,

    [Parameter()]
    [switch]$SkipAppBuild,

    [Parameter()]
    [switch]$IncludeMsix
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$releaseBuild = Join-Path $repositoryRoot 'out\build\nativeshift-release'
$distributionRoot = Join-Path $repositoryRoot 'out\packages'
$stagingRoot = Join-Path $repositoryRoot 'out\package-staging'
$appProject = Join-Path $repositoryRoot 'src\NativeShift.App\NativeShift.App.vcxproj'
$vcpkgInstalled = Join-Path $releaseBuild 'vcpkg_installed\x64-windows'

. (Join-Path $PSScriptRoot 'initialize-build-environment.ps1')
Initialize-NativeBuildEnvironment -Architecture $Architecture

if (Test-Path -LiteralPath $distributionRoot) {
    Remove-Item -LiteralPath $distributionRoot -Recurse -Force
}
if (-not $SkipAppBuild -and (Test-Path -LiteralPath $stagingRoot)) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $distributionRoot, $stagingRoot -Force |
    Out-Null
if ($SkipAppBuild) {
    foreach ($name in @('notices', 'cli', 'portable', 'symbols')) {
        $path = Join-Path $stagingRoot $name
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
}

function Invoke-Checked {
    param([Parameter(Mandatory)][string]$Program, [Parameter(Mandatory)][string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Content
    )
    [IO.File]::WriteAllText(
        $Path,
        $Content,
        [Text.UTF8Encoding]::new($false)
    )
}

function Get-VcpkgRuntimeDependencies {
    param(
        [Parameter(Mandatory)][string[]]$Binaries,
        [Parameter(Mandatory)][string]$RuntimeDirectory
    )
    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbin) {
        throw 'dumpbin.exe was not found.'
    }
    $available = @{}
    Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' -File |
        ForEach-Object { $available[$_.Name.ToLowerInvariant()] = $_.FullName }

    $queue = [Collections.Generic.Queue[string]]::new()
    foreach ($binary in $Binaries) {
        $queue.Enqueue($binary)
    }
    $resolved = @{}
    while ($queue.Count -gt 0) {
        $binary = $queue.Dequeue()
        $dependencies = & $dumpbin.Source /dependents $binary 2>$null
        if ($LASTEXITCODE -ne 0) {
            throw "dumpbin failed for $binary."
        }
        foreach ($line in $dependencies) {
            if ($line -notmatch '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
                continue
            }
            $name = $Matches[1].ToLowerInvariant()
            if ($available.ContainsKey($name) -and
                -not $resolved.ContainsKey($name)) {
                $resolved[$name] = $available[$name]
                $queue.Enqueue($available[$name])
            }
        }
    }
    return @($resolved.Values | Sort-Object)
}

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
            Select-Object -First 1
        if ($path) {
            return $path
        }
    }
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    throw 'MSBuild was not found. Install the Visual Studio Windows application development workload.'
}

function Find-MakeAppx {
    $command = Get-Command makeappx.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    $sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $candidate = Get-ChildItem -LiteralPath $sdkBin -Filter makeappx.exe `
        -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object FullName -Match '\\x64\\makeappx\.exe$' |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($candidate) {
        return $candidate.FullName
    }
    throw 'makeappx.exe was not found.'
}

if (-not $SkipBuild) {
    Invoke-Checked -Program 'cmake.exe' -Arguments @('--preset', 'release')
    Invoke-Checked -Program 'cmake.exe' -Arguments @('--build', '--preset', 'release', '--parallel', '4')
}

$cliExecutable = Join-Path $releaseBuild 'src\NativeShift.Cli\nativeshift-cli.exe'
$bridgeDll = Join-Path $releaseBuild 'src\NativeShift.GuiBridge\NativeShift.GuiBridge.dll'
foreach ($required in @($cliExecutable, $bridgeDll, $vcpkgInstalled)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required Release output is missing: $required"
    }
}

$msbuild = Find-MSBuild
$msixOutput = Join-Path $stagingRoot 'msix\'
$portableOutput = Join-Path $stagingRoot 'portable-app\'
$portableIntermediate = Join-Path $stagingRoot 'portable-obj\'
$runtimeDlls = Get-VcpkgRuntimeDependencies `
    -Binaries @($cliExecutable, $bridgeDll) `
    -RuntimeDirectory (Join-Path $vcpkgInstalled 'bin')
if ($runtimeDlls.Count -eq 0) {
    throw 'No vcpkg runtime dependency closure was resolved.'
}

if (-not $SkipAppBuild) {
    if ($IncludeMsix) {
        Invoke-Checked -Program $msbuild -Arguments @(
            $appProject,
            '/restore',
            '/m:1',
            '/v:minimal',
            '/p:Configuration=Release',
            "/p:Platform=$Architecture",
            '/p:AppxBundle=Never',
            '/p:GenerateAppxPackageOnBuild=true',
            '/p:UapAppxPackageBuildMode=SideloadOnly',
            '/p:AppxPackageSigningEnabled=false',
            "/p:AppxPackageDir=$msixOutput"
        )
    }

    Invoke-Checked -Program $msbuild -Arguments @(
        $appProject,
        '/restore',
        '/m:1',
        '/v:minimal',
        '/p:Configuration=Release',
        "/p:Platform=$Architecture",
        '/p:WindowsPackageType=None',
        '/p:AppxPackage=false',
        '/p:WindowsAppSDKSelfContained=true',
        '/p:WholeProgramOptimization=false',
        "/p:IntDir=$portableIntermediate",
        "/p:OutDir=$portableOutput"
    )
}

$msixPath = $null
if ($IncludeMsix) {
    $msixCandidate = Get-ChildItem -LiteralPath $msixOutput -Recurse -File |
        Where-Object {
            $_.Extension -in @('.msix', '.appx') -and
            $_.Name -match '^NativeShift\.App_' -and
            $_.FullName -notmatch '[\\/]Dependencies[\\/]'
        } |
        Sort-Object FullName |
        Select-Object -First 1
    if (-not $msixCandidate) {
        throw 'MSBuild did not produce an MSIX package.'
    }
    $msixName = "NativeShift-$Version-windows-$Architecture.msix"
    $msixPath = Join-Path $distributionRoot $msixName
    Copy-Item -LiteralPath $msixCandidate.FullName -Destination $msixPath
}

$licenseStage = Join-Path $stagingRoot 'notices'
New-Item -ItemType Directory -Path (Join-Path $licenseStage 'LICENSES') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination $licenseStage
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'THIRD_PARTY_NOTICES.md') -Destination $licenseStage

$shareRoot = Join-Path $vcpkgInstalled 'share'
Get-ChildItem -LiteralPath $shareRoot -Directory | ForEach-Object {
    $copyright = Join-Path $_.FullName 'copyright'
    if (Test-Path -LiteralPath $copyright) {
        Copy-Item -LiteralPath $copyright -Destination (
            Join-Path $licenseStage "LICENSES\$($_.Name).txt"
        )
    }
}
if ((Get-ChildItem -LiteralPath (Join-Path $licenseStage 'LICENSES') -File).Count -eq 0) {
    throw 'No resolved vcpkg license texts were found.'
}

if ($IncludeMsix) {
    $makeAppx = Find-MakeAppx
    $expandedMsix = Join-Path $stagingRoot 'msix-expanded'
    if (Test-Path -LiteralPath $expandedMsix) {
        Remove-Item -LiteralPath $expandedMsix -Recurse -Force
    }
    Invoke-Checked -Program $makeAppx -Arguments @(
        'unpack', '/p', $msixPath, '/d', $expandedMsix, '/o'
    )
    Copy-Item -Path (Join-Path $licenseStage '*') `
        -Destination $expandedMsix -Recurse -Force
    Copy-Item -LiteralPath $bridgeDll -Destination $expandedMsix -Force
    $runtimeDlls | Copy-Item -Destination $expandedMsix -Force
    Remove-Item -LiteralPath $msixPath -Force
    Invoke-Checked -Program $makeAppx -Arguments @(
        'pack', '/d', $expandedMsix, '/p', $msixPath, '/o'
    )
}

$cliStage = Join-Path $stagingRoot 'cli'
New-Item -ItemType Directory -Path $cliStage | Out-Null
Copy-Item -LiteralPath $cliExecutable -Destination $cliStage
$runtimeDlls | Copy-Item -Destination $cliStage
Copy-Item -Path (Join-Path $licenseStage '*') -Destination $cliStage -Recurse

$portableStage = Join-Path $stagingRoot 'portable'
New-Item -ItemType Directory -Path $portableStage | Out-Null
Get-ChildItem -LiteralPath $portableOutput -Force |
    Copy-Item -Destination $portableStage -Recurse -Force
Get-ChildItem -LiteralPath $portableStage -File -Recurse |
    Where-Object {
        $_.Extension -in @('.pdb', '.lib', '.exp') -or
        $_.Name -match '^(gtest|gmock|pkgconf).*\.(?:dll|exe)$'
    } |
    Remove-Item -Force
Copy-Item -LiteralPath $bridgeDll -Destination $portableStage -Force
$runtimeDlls | Copy-Item -Destination $portableStage -Force
Copy-Item -Path (Join-Path $licenseStage '*') -Destination $portableStage -Recurse

function New-Zip {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    $items = Get-ChildItem -LiteralPath $Source -Force
    if (-not $items) {
        throw "Cannot create an empty archive from $Source."
    }
    Compress-Archive -Path (Join-Path $Source '*') -DestinationPath $Destination -CompressionLevel Optimal
}

New-Zip -Source $portableStage -Destination (
    Join-Path $distributionRoot "NativeShift-$Version-portable-windows-$Architecture.zip"
)
New-Zip -Source $cliStage -Destination (
    Join-Path $distributionRoot "NativeShift-CLI-$Version-windows-$Architecture.zip"
)

$symbolsStage = Join-Path $stagingRoot 'symbols'
New-Item -ItemType Directory -Path $symbolsStage | Out-Null
Get-ChildItem -LiteralPath $releaseBuild -Filter '*.pdb' -File -Recurse |
    Where-Object { $_.FullName -notmatch '\\vcpkg_installed\\' } |
    Copy-Item -Destination $symbolsStage
Get-ChildItem -LiteralPath $portableOutput -Filter '*.pdb' -File |
    Copy-Item -Destination $symbolsStage
if (-not (Get-ChildItem -LiteralPath $symbolsStage -File)) {
    throw 'No project symbol files were found.'
}
New-Zip -Source $symbolsStage -Destination (
    Join-Path $distributionRoot "NativeShift-Symbols-$Version-windows-$Architecture.zip"
)

$vcpkgStatus = Join-Path (Split-Path -Parent $vcpkgInstalled) 'vcpkg\status'
$packages = @()
if (Test-Path -LiteralPath $vcpkgStatus) {
    $paragraphs = (Get-Content -LiteralPath $vcpkgStatus -Raw) -split "(?:\r?\n){2,}"
    foreach ($paragraph in $paragraphs) {
        $name = [regex]::Match($paragraph, '(?m)^Package:\s*(.+)$')
        $resolvedVersion = [regex]::Match($paragraph, '(?m)^Version:\s*(.+)$')
        if ($name.Success -and $resolvedVersion.Success) {
            $portVersion = [regex]::Match($paragraph, '(?m)^Port-Version:\s*(\d+)$')
            $versionInfo = $resolvedVersion.Groups[1].Value
            if ($portVersion.Success -and $portVersion.Groups[1].Value -ne '0') {
                $versionInfo += "#$($portVersion.Groups[1].Value)"
            }
            $packages += [ordered]@{
                SPDXID = "SPDXRef-Package-vcpkg-$($name.Groups[1].Value -replace '[^A-Za-z0-9.-]', '-')"
                name = $name.Groups[1].Value
                versionInfo = $versionInfo
                downloadLocation = 'NOASSERTION'
                filesAnalyzed = $false
                licenseConcluded = 'NOASSERTION'
                licenseDeclared = 'NOASSERTION'
            }
        }
    }
}

$nuget = Select-Xml -LiteralPath $appProject -XPath "/*[local-name()='Project']/*[local-name()='ItemGroup']/*[local-name()='PackageReference']"
foreach ($node in $nuget) {
    $packages += [ordered]@{
        SPDXID = "SPDXRef-Package-nuget-$($node.Node.Include -replace '[^A-Za-z0-9.-]', '-')"
        name = $node.Node.Include
        versionInfo = $node.Node.Version
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
    }
}

$created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$sbom = [ordered]@{
    spdxVersion = 'SPDX-2.3'
    dataLicense = 'CC0-1.0'
    SPDXID = 'SPDXRef-DOCUMENT'
    name = "NativeShift-$Version-windows-$Architecture"
    documentNamespace = "https://github.com/Koon-Kiat/NativeShift/sbom/$Version/$([guid]::NewGuid())"
    creationInfo = [ordered]@{
        created = $created
        creators = @('Tool: NativeShift-build-packages')
    }
    packages = @(
        [ordered]@{
            SPDXID = 'SPDXRef-Package-NativeShift'
            name = 'NativeShift'
            versionInfo = $Version
            downloadLocation = 'NOASSERTION'
            filesAnalyzed = $false
            licenseConcluded = 'MIT'
            licenseDeclared = 'MIT'
        }
    ) + $packages
}
$sbomPath = Join-Path $distributionRoot "NativeShift-$Version-sbom.spdx.json"
Write-Utf8NoBom -Path $sbomPath -Content (
    $sbom | ConvertTo-Json -Depth 8
)

$artifactFiles = Get-ChildItem -LiteralPath $distributionRoot -File | Sort-Object Name
$manifest = [ordered]@{
    schema_version = 1
    application = 'NativeShift'
    version = $Version
    architecture = $Architecture
    created_utc = $created
    git_commit = (git -C $repositoryRoot rev-parse HEAD)
    signed = $false
    publisher = 'CN=NativeShift Development'
    ffmpeg_features = @(
        'amf', 'aom', 'avcodec', 'avformat', 'mp3lame', 'nvcodec',
        'openh264', 'opus', 'qsv', 'swresample', 'swscale', 'vorbis', 'vpx'
    )
    artifacts = @($artifactFiles | ForEach-Object {
        [ordered]@{
            name = $_.Name
            size = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}
$manifestPath = Join-Path $distributionRoot "NativeShift-$Version-release-manifest.json"
Write-Utf8NoBom -Path $manifestPath -Content (
    $manifest | ConvertTo-Json -Depth 6
)

$checksumFiles = Get-ChildItem -LiteralPath $distributionRoot -File |
    Where-Object Name -ne 'SHA256SUMS' |
    Sort-Object Name
$checksumLines = foreach ($file in $checksumFiles) {
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($file.Name)"
}
$checksumLines | Set-Content -LiteralPath (Join-Path $distributionRoot 'SHA256SUMS') -Encoding ascii

Write-Host "Created NativeShift $Version package candidates in $distributionRoot"
