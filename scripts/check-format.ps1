[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$clangFormat = Get-Command clang-format.exe -ErrorAction SilentlyContinue
if (-not $clangFormat) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $clangFormat = & $vswhere -latest -products * `
            -find 'VC\Tools\Llvm\x64\bin\clang-format.exe' |
            Select-Object -First 1
    }
}
if (-not $clangFormat -and
    (Test-Path -LiteralPath 'C:\Program Files\Microsoft Visual Studio')) {
    $clangFormat = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' `
        -Filter clang-format.exe -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1
}
if (-not $clangFormat) {
    throw 'clang-format.exe was not found.'
}
$program = if ($clangFormat -is [string]) {
    $clangFormat
} elseif ($clangFormat.Source) {
    $clangFormat.Source
} else {
    $clangFormat.FullName
}

$relativeFiles = & git -C $repositoryRoot ls-files --cached --others --exclude-standard -- `
    'src/**/*.cpp' 'src/**/*.h' 'src/**/*.hpp' `
    'tests/**/*.cpp' 'tests/**/*.h' 'tests/**/*.hpp' `
    'benchmarks/**/*.cpp' 'benchmarks/**/*.h' 'benchmarks/**/*.hpp'
if ($LASTEXITCODE -ne 0) {
    throw 'git ls-files failed while collecting project-owned C++.'
}
$files = @($relativeFiles | ForEach-Object { Join-Path $repositoryRoot $_ })
if ($files.Count -eq 0) {
    throw 'No project-owned C++ files were found.'
}
& $program --dry-run --Werror @files
if ($LASTEXITCODE -ne 0) {
    throw 'Project-owned C++ is not clang-format clean.'
}
Write-Host "clang-format verified $($files.Count) files."
