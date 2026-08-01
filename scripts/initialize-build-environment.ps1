function Initialize-NativeBuildEnvironment {
    [CmdletBinding()]
    param(
        [Parameter()]
        [ValidateSet('x64')]
        [string]$Architecture = 'x64'
    )

    function Set-NativeProcessPath {
        param([Parameter(Mandatory)][string]$Value)

        [Environment]::SetEnvironmentVariable(
            'PATH', $null, [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable(
            'Path', $null, [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable(
            'Path', $Value, [EnvironmentVariableTarget]::Process)
    }

    function Add-NativeProcessPath {
        param([Parameter(Mandatory)][string]$Directory)

        $current = [Environment]::GetEnvironmentVariable(
            'Path', [EnvironmentVariableTarget]::Process)
        Set-NativeProcessPath -Value "$Directory;$current"
    }

    $initialPath = [Environment]::GetEnvironmentVariable(
        'Path', [EnvironmentVariableTarget]::Process)
    Set-NativeProcessPath -Value $initialPath

    $installation = $null
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} `
            'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) {
            throw 'vswhere.exe was not found.'
        }
        $installation = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if (-not $installation) {
            throw 'A Visual Studio C++ toolchain was not found.'
        }
        $developerCommand = Join-Path $installation `
            'Common7\Tools\VsDevCmd.bat'
        $environment = & cmd.exe /d /s /c `
            "`"$developerCommand`" -arch=$Architecture -host_arch=x64 >nul && set"
        if ($LASTEXITCODE -ne 0) {
            throw 'Visual Studio developer environment initialization failed.'
        }
        foreach ($line in $environment) {
            $separator = $line.IndexOf('=')
            if ($separator -le 0) {
                continue
            }
            $name = $line.Substring(0, $separator)
            $value = $line.Substring($separator + 1)
            if ($name -ieq 'Path') {
                Set-NativeProcessPath -Value $value
            } else {
                Set-Item -Path "Env:$name" -Value $value
            }
        }
    }

    if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
        if (-not $installation) {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} `
                'Microsoft Visual Studio\Installer\vswhere.exe'
            if (Test-Path -LiteralPath $vswhere) {
                $installation = & $vswhere -latest -products * `
                    -property installationPath
            }
        }
        $bundledCMake = if ($installation) {
            Join-Path $installation `
                'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        }
        if (-not $bundledCMake -or
            -not (Test-Path -LiteralPath $bundledCMake)) {
            throw 'cmake.exe was not found.'
        }
        Add-NativeProcessPath -Directory (Split-Path -Parent $bundledCMake)
    }

    if (-not (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} `
            'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) {
            throw 'vswhere.exe was not found while locating dumpbin.exe.'
        }
        $dumpbin = & $vswhere -latest -products * `
            -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' |
            Select-Object -First 1
        if (-not $dumpbin) {
            throw 'dumpbin.exe was not found in the active Visual Studio installation.'
        }
        Add-NativeProcessPath -Directory (Split-Path -Parent $dumpbin)
    }
}
