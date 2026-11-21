<#
.SYNOPSIS
    Downloads the pinned Windows build tools into .tools/.

.DESCRIPTION
    Ironthorn needs CMake and Ninja to build, and clang-format and clang-tidy to lint. This
    script fetches pinned, checksum-verified portable builds into the repository-local
    .tools/ directory so nothing is installed system-wide. Visual Studio 2022 with the "Desktop development with C++" workload must
    already be installed.

    Linux and macOS developers should install cmake and ninja from their package manager
    instead (see docs/development/building.md).

.PARAMETER Force
    Re-download tools even when the pinned version is already present.

.EXAMPLE
    ./scripts/bootstrap.ps1
#>
[CmdletBinding()]
param(
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $repoRoot '.tools'
$downloadDir = Join-Path $toolsDir 'downloads'

# Pinned tool versions. Update the version, URL and SHA-256 together.
$archiveTools = @(
    @{
        Name            = 'cmake'
        Version         = '4.4.4'
        Url             = 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip'
        Sha256          = 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536'
        StripComponents = 1
    },
    @{
        Name            = 'ninja'
        Version         = '1.13.2'
        Url             = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
        Sha256          = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'
        StripComponents = 0
    },
    # clang-format and clang-tidy are pinned so every machine and CI lint with the same
    # version; the compiler (clang-cl) still comes from Visual Studio. These are the
    # standalone LLVM builds published on PyPI. A wheel is a zip file, so no Python is
    # needed: stripping two levels leaves bin/ (and lib/clang/<major>/include for
    # clang-tidy's builtin headers) in the install directory.
    @{
        Name            = 'clang-format'
        Version         = '23.1.4'
        Url             = 'https://files.pythonhosted.org/packages/82/c2/a32b7ed0d037d3fc497f494808d3926f050fa3c7145a2205ee51086d5c90/clang_format-23.1.4-py2.py3-none-win_amd64.whl'
        Sha256          = 'bd372a6c247c22b5d7a5a748aa66b88476aec946bd8e644f8a68b990c90714a8'
        StripComponents = 2
    },
    @{
        Name            = 'clang-tidy'
        Version         = '23.1.4'
        Url             = 'https://files.pythonhosted.org/packages/c0/49/b2adbdedbe430d0cce9b354a679d57e28692ce2fb59a575486784d438125/clang_tidy-23.1.4-py2.py3-none-win_amd64.whl'
        Sha256          = '8107e05930f53dc477dbb5fb8108d7c10464176fc95d3e58f4ee7317887aed59'
        StripComponents = 2
    }
)

function Get-VerifiedDownload {
    param([string]$Url, [string]$Sha256, [string]$Destination)

    if (-not (Test-Path $Destination)) {
        Write-Host "  downloading $Url"
        Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
    }

    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Destination).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) {
        Remove-Item -LiteralPath $Destination -Force
        throw "Checksum mismatch for $Url. Expected $Sha256 but got $actual. The download was deleted."
    }
}

function Test-ToolInstalled {
    param([string]$InstallDir, [string]$Version)

    $stamp = Join-Path $InstallDir '.version'
    return (Test-Path $stamp) -and ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $Version)
}

function Set-ToolInstalled {
    param([string]$InstallDir, [string]$Version)

    Set-Content -LiteralPath (Join-Path $InstallDir '.version') -Value $Version -Encoding ascii
}

function Install-ArchiveTool {
    param([hashtable]$Tool)

    $installDir = Join-Path $toolsDir $Tool.Name
    if (-not $Force -and (Test-ToolInstalled -InstallDir $installDir -Version $Tool.Version)) {
        Write-Host "$($Tool.Name) $($Tool.Version) is already installed."
        return
    }

    Write-Host "Installing $($Tool.Name) $($Tool.Version)..."
    $archive = Join-Path $downloadDir ("{0}-{1}.zip" -f $Tool.Name, $Tool.Version)
    Get-VerifiedDownload -Url $Tool.Url -Sha256 $Tool.Sha256 -Destination $archive

    if (Test-Path $installDir) {
        Remove-Item -LiteralPath $installDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null

    # bsdtar ships with Windows 10+ and extracts large zip files far faster than Expand-Archive.
    & tar.exe -xf $archive -C $installDir --strip-components $Tool.StripComponents
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to extract $archive (tar exit code $LASTEXITCODE)."
    }

    Set-ToolInstalled -InstallDir $installDir -Version $Tool.Version
}

New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null

foreach ($tool in $archiveTools) {
    Install-ArchiveTool -Tool $tool
}

Write-Host ''
Write-Host 'Bootstrap complete. Use scripts/dev.ps1 to configure, build and test.'
