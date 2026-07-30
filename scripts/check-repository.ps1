[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

Push-Location $repositoryRoot
try {
    & git diff --check
    if ($LASTEXITCODE -ne 0) {
        throw 'git diff --check found whitespace errors.'
    }

    $tracked = @(& git ls-files)
    $forbiddenNames = $tracked | Where-Object {
        $_ -match '(?i)(^|/)(out|vcpkg_installed|packages?|logs?|temp)/' -or
        $_ -match '(?i)\.(pfx|p12|pem|key|cer|crt|user|suo|ilk|obj|pch|idb)$' -or
        $_ -match '(?i)(^|/)\.env(?:\.|$)'
    }
    if ($forbiddenNames) {
        throw "Forbidden tracked file: $($forbiddenNames[0])"
    }

    foreach ($relative in $tracked) {
        $path = Join-Path $repositoryRoot $relative
        if ((Get-Item -LiteralPath $path).Length -gt 10MB) {
            throw "Tracked file exceeds 10 MiB: $relative"
        }
        if ($relative -match '(?i)\.(exe|dll|lib|pdb|zip|msix|appx)$') {
            throw "Generated binary is tracked: $relative"
        }
    }

    $conflicts = & git grep -n -E '^(<<<<<<<|=======|>>>>>>>)' -- ':!scripts/check-repository.ps1'
    if ($LASTEXITCODE -eq 0 -and $conflicts) {
        throw "Merge-conflict marker found: $($conflicts[0])"
    }

    $obsoleteNaming = & git grep -n -i -E --untracked --exclude-standard `
        -e 'docs/naming\.md|naming\.md' 2>$null
    if ($LASTEXITCODE -eq 0 -and $obsoleteNaming) {
        throw 'The removed naming document is referenced.'
    }

    Get-ChildItem -LiteralPath $repositoryRoot -Filter '*.json' -File -Recurse |
        Where-Object FullName -NotMatch '\\(out|\.git)\\' |
        ForEach-Object {
            Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json | Out-Null
        }

    Get-ChildItem -LiteralPath $repositoryRoot -File -Recurse |
        Where-Object {
            $_.FullName -notmatch '\\(out|\.git)\\' -and
            $_.Extension -in @('.xml', '.xaml', '.appxmanifest', '.vcxproj')
        } |
        ForEach-Object {
            [xml](Get-Content -LiteralPath $_.FullName -Raw) | Out-Null
        }

    $markdownFiles = Get-ChildItem -LiteralPath $repositoryRoot -Filter '*.md' -File -Recurse |
        Where-Object FullName -NotMatch '\\(out|\.git)\\'
    foreach ($file in $markdownFiles) {
        $content = Get-Content -LiteralPath $file.FullName -Raw
        foreach ($match in [regex]::Matches($content, '\[[^\]]+\]\((?!https?://|#|mailto:)([^)]+)\)')) {
            $targetText = $match.Groups[1].Value.Split('#')[0]
            if ([string]::IsNullOrWhiteSpace($targetText)) {
                continue
            }
            $target = Join-Path $file.DirectoryName ([uri]::UnescapeDataString($targetText))
            if (-not (Test-Path -LiteralPath $target)) {
                throw "Broken local Markdown link in $($file.FullName): $targetText"
            }
        }
    }

    $secretPatterns = @(
        '-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----',
        'gh[pousr]_[A-Za-z0-9_]{20,}',
        'AKIA[0-9A-Z]{16}'
    )
    foreach ($pattern in $secretPatterns) {
        $matches = & git grep -n -E --untracked --exclude-standard `
            -e $pattern -- ':!scripts/check-repository.ps1'
        if ($LASTEXITCODE -gt 1) {
            throw 'git grep failed while scanning for secret-like content.'
        }
        if ($LASTEXITCODE -eq 0 -and $matches) {
            throw "Secret-like tracked content matched a prohibited pattern."
        }
    }

    Write-Host 'Repository hygiene checks passed.'
    $global:LASTEXITCODE = 0
} finally {
    Pop-Location
}
