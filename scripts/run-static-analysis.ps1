[CmdletBinding()]
param(
    [Parameter()]
    [switch]$SkipCppcheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$analysisRoot = Join-Path $repositoryRoot 'out\analysis'
New-Item -ItemType Directory -Path $analysisRoot -Force | Out-Null

. (Join-Path $PSScriptRoot 'initialize-build-environment.ps1')
Initialize-NativeBuildEnvironment

foreach ($tool in @('cmake.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool was not found."
    }
}
if (-not $SkipCppcheck -and
    -not (Get-Command cppcheck.exe -ErrorAction SilentlyContinue)) {
    throw 'cppcheck.exe was not found. Install the pinned version or use -SkipCppcheck for a documented local partial run.'
}
$clangTidy = Get-Command clang-tidy.exe -ErrorAction SilentlyContinue
if (-not $clangTidy) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $clangTidy = & $vswhere -latest -products * `
            -find 'VC\Tools\Llvm\x64\bin\clang-tidy.exe' |
            Select-Object -First 1
    }
}
if (-not $clangTidy) {
    throw 'clang-tidy.exe was not found.'
}
$clangTidyPath = if ($clangTidy -is [string]) {
    $clangTidy
} elseif ($clangTidy.Source) {
    $clangTidy.Source
} else {
    $clangTidy.FullName
}
if (-not $env:VCPKG_ROOT) {
    throw 'VCPKG_ROOT must identify the pinned vcpkg checkout.'
}

function Invoke-Checked {
    param([Parameter(Mandatory)][string]$Program, [Parameter(Mandatory)][string[]]$Arguments)
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Program @Arguments
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) {
        throw "$Program failed with exit code $exitCode."
    }
}

$toolchain = Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'
$tidyBuild = Join-Path $repositoryRoot 'out\build\nativeshift-clang-tidy'
Invoke-Checked -Program 'cmake.exe' -Arguments @(
    '-S', $repositoryRoot,
    '-B', $tidyBuild,
    '-G', 'Ninja',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DVCPKG_TARGET_TRIPLET=x64-windows',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    '-DNATIVESHIFT_BUILD_BENCHMARKS=OFF',
    "-DCMAKE_CXX_CLANG_TIDY=$clangTidyPath;--use-color=false;--extra-arg=/EHsc"
)
Invoke-Checked -Program 'cmake.exe' -Arguments @(
    '--build', $tidyBuild,
    '--target', 'NativeShiftGuiBridge', 'NativeShift.Cli'
)

$compileCommands = Join-Path $tidyBuild 'compile_commands.json'
if (-not $SkipCppcheck) {
    $cppcheckOutput = Join-Path $analysisRoot 'cppcheck.txt'
    & cppcheck.exe `
        "--project=$compileCommands" `
        '--enable=warning,performance,portability' `
        '--error-exitcode=2' `
        '--inline-suppr' `
        "--suppressions-list=$(Join-Path $repositoryRoot 'cppcheck-suppressions.txt')" `
        '--template={file}:{line}:{severity}:{id}:{message}' `
        2> $cppcheckOutput
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $cppcheckOutput
        throw "cppcheck failed with exit code $LASTEXITCODE."
    }
}

$msvcBuild = Join-Path $repositoryRoot 'out\build\nativeshift-msvc-analyze'
Invoke-Checked -Program 'cmake.exe' -Arguments @(
    '-S', $repositoryRoot,
    '-B', $msvcBuild,
    '-G', 'Ninja',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DVCPKG_TARGET_TRIPLET=x64-windows',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DNATIVESHIFT_BUILD_BENCHMARKS=OFF',
    '-DCMAKE_CXX_FLAGS=/analyze /analyze:external-'
)
Invoke-Checked -Program 'cmake.exe' -Arguments @(
    '--build', $msvcBuild,
    '--target', 'NativeShiftGuiBridge', 'NativeShift.Cli'
)

if ($SkipCppcheck) {
    Write-Warning 'Cppcheck was skipped; clang-tidy and MSVC /analyze passed.'
} else {
    Write-Host 'clang-tidy, cppcheck, and MSVC /analyze passed.'
}
