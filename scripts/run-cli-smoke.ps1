[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$preset = $Configuration.ToLowerInvariant()
$cli = Join-Path $repositoryRoot "out\build\nativeshift-$preset\src\NativeShift.Cli\nativeshift-cli.exe"
$fixtureGenerator = Join-Path $repositoryRoot "out\build\nativeshift-$preset\tests\NativeShift.Media.Tests.exe"
if (-not (Test-Path -LiteralPath $cli)) {
    throw "CLI does not exist: $cli"
}
if (-not (Test-Path -LiteralPath $fixtureGenerator)) {
    throw "Media fixture generator does not exist: $fixtureGenerator"
}

foreach ($arguments in @(
    @('--version'),
    @('--help'),
    @('--list-formats'),
    @('--list-codecs'),
    @('--list-hardware'),
    @('--list-presets')
)) {
    & $cli @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "CLI smoke command failed: $($arguments -join ' ')"
    }
}

$json = & $cli --list-formats --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $json.application -ne 'NativeShift') {
    throw 'CLI JSON output is invalid or unstable.'
}

& $cli --definitely-invalid | Out-Null
if ($LASTEXITCODE -ne 2) {
    throw "Invalid CLI command returned $LASTEXITCODE instead of 2."
}

$smokeRoot = Join-Path ([IO.Path]::GetTempPath()) (
    "NativeShift-cli-smoke-$([Guid]::NewGuid().ToString('N'))"
)
New-Item -ItemType Directory -Path $smokeRoot | Out-Null
try {
    $bitmap = New-Object byte[] 70
    [Text.Encoding]::ASCII.GetBytes('BM').CopyTo($bitmap, 0)
    [BitConverter]::GetBytes([uint32]70).CopyTo($bitmap, 2)
    [BitConverter]::GetBytes([uint32]54).CopyTo($bitmap, 10)
    [BitConverter]::GetBytes([uint32]40).CopyTo($bitmap, 14)
    [BitConverter]::GetBytes([int32]2).CopyTo($bitmap, 18)
    [BitConverter]::GetBytes([int32]2).CopyTo($bitmap, 22)
    [BitConverter]::GetBytes([uint16]1).CopyTo($bitmap, 26)
    [BitConverter]::GetBytes([uint16]24).CopyTo($bitmap, 28)
    [BitConverter]::GetBytes([uint32]16).CopyTo($bitmap, 34)
    [byte[]]$pixels = @(
        0, 0, 255, 0, 255, 0, 0, 0,
        255, 0, 0, 255, 255, 255, 0, 0
    )
    $pixels.CopyTo($bitmap, 54)
    $bitmapPath = Join-Path $smokeRoot 'source.bmp'
    [IO.File]::WriteAllBytes($bitmapPath, $bitmap)

    $wavPath = Join-Path $smokeRoot 'source.wav'
    $stream = [IO.File]::Create($wavPath)
    $writer = New-Object IO.BinaryWriter($stream)
    try {
        $sampleRate = 8000
        $sampleCount = 800
        $dataSize = $sampleCount * 2
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([uint32](36 + $dataSize))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $writer.Write([uint32]16)
        $writer.Write([uint16]1)
        $writer.Write([uint16]1)
        $writer.Write([uint32]$sampleRate)
        $writer.Write([uint32]($sampleRate * 2))
        $writer.Write([uint16]2)
        $writer.Write([uint16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([uint32]$dataSize)
        for ($sample = 0; $sample -lt $sampleCount; ++$sample) {
            $value = [int16]([Math]::Sin($sample * 0.125663706) * 6000)
            $writer.Write($value)
        }
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }

    $videoPath = Join-Path $smokeRoot 'source.mkv'
    & $fixtureGenerator --write-smoke-video $videoPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $videoPath)) {
        throw 'The legal generated video fixture could not be created.'
    }

    $conversions = @(
        @($bitmapPath, '--to', 'png', '--output', (Join-Path $smokeRoot 'image.png'), '--json'),
        @($wavPath, '--to', 'flac', '--output', (Join-Path $smokeRoot 'audio.flac'), '--json'),
        @(
            $videoPath, '--to', 'mp4',
            '--output', (Join-Path $smokeRoot 'video.mp4'),
            '--video-codec', 'h264',
            '--video-audio-codec', 'aac',
            '--hardware', 'software',
            '--json'
        )
    )
    foreach ($conversion in $conversions) {
        $result = & $cli @conversion | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or
            $result.application -ne 'NativeShift' -or
            $result.results.Count -ne 1 -or
            $result.results[0].status -ne 'success') {
            throw "CLI conversion failed: $($conversion -join ' ')"
        }
    }

    & $cli $bitmapPath --to png `
        --output (Join-Path $smokeRoot 'image.png') `
        --conflict unique --quiet
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath (Join-Path $smokeRoot 'image (1).png'))) {
        throw 'CLI unique-name conflict handling failed.'
    }
} finally {
    if (Test-Path -LiteralPath $smokeRoot) {
        Remove-Item -LiteralPath $smokeRoot -Recurse -Force
    }
}

Write-Host "CLI $Configuration smoke tests passed."
$global:LASTEXITCODE = 0
