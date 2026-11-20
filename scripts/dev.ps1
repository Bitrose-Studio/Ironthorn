<#
.SYNOPSIS
    Windows developer entry point: configure, build, test, format and lint Ironthorn.

.DESCRIPTION
    Loads the MSVC build environment, puts the bootstrapped tools from .tools/ on PATH and
    forwards to CMake/CTest. Linux developers call the CMake presets directly
    (docs/development/building.md).

.PARAMETER Command
    configure     Generate the build tree for the selected preset.
    build         Build one target (-Target) or everything; configures first when needed.
    test          Run CTest for the selected preset and configuration.
    format        Reformat all C++ sources in place with clang-format.
    format-check  Fail when any C++ source is not clang-format clean.
    tidy          Run clang-tidy over the engine sources (windows-clang preset).
    verify        The pre-commit gate: format-check, MSVC Debug + development + Release
                  builds and tests, clang-cl build and tests, and tidy. One PASS/FAIL line
                  per step.
    clean         Delete the build tree of the selected preset.

.EXAMPLE
    ./scripts/dev.ps1 build
    ./scripts/dev.ps1 test -Filter foundation
    ./scripts/dev.ps1 verify
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('configure', 'build', 'test', 'format', 'format-check', 'tidy', 'verify', 'clean')]
    [string]$Command,

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',

    [ValidateSet('windows', 'windows-clang')]
    [string]$Preset = 'windows',

    # build: target name. Empty builds everything.
    [string]$Target = '',

    # test: CTest name regex (-R). tidy: regex matched against source file paths.
    [string]$Filter = '',

    # test: skip tests with this CTest label (-LE), e.g. 'gpu' on machines without a GPU.
    [string]$ExcludeLabel = '',

    # configure: extra CMake arguments.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $repoRoot '.tools'
if ($Command -eq 'tidy' -and -not $PSBoundParameters.ContainsKey('Preset')) {
    $Preset = 'windows-clang'
}

$sourceDirectories = @('engine', 'editor', 'tests', 'tools')
$lintDirectories = @('engine', 'editor', 'tools')

function Get-BuildDir([string]$PresetName) {
    return Join-Path $repoRoot "build\$PresetName"
}

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe was not found. Install Visual Studio 2022 with the "Desktop development with C++" workload.'
    }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) {
        throw 'No Visual Studio installation with the MSVC x64 toolset was found.'
    }
    return $path
}

function Import-BuildEnvironment {
    foreach ($tool in @('cmake\bin', 'ninja')) {
        $toolPath = Join-Path $toolsDir $tool
        if (Test-Path $toolPath) {
            $env:PATH = "$toolPath;$env:PATH"
        }
    }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -or -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        throw 'cmake or ninja was not found. Run scripts/bootstrap.ps1 first.'
    }
    if ($env:VSCMD_VER) {
        return # already inside a Visual Studio developer prompt
    }

    $vcvars = Join-Path (Find-VisualStudio) 'VC\Auxiliary\Build\vcvars64.bat'
    $dump = & cmd.exe /c "`"$vcvars`" > nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to load the MSVC environment from $vcvars."
    }
    foreach ($line in $dump) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
    # clang-cl ships with Visual Studio's "C++ Clang tools" component.
    $llvm = Join-Path (Find-VisualStudio) 'VC\Tools\Llvm\x64\bin'
    if (Test-Path $llvm) {
        $env:PATH = "$env:PATH;$llvm"
    }
}

function Invoke-Checked {
    param([string]$Executable, [string[]]$ArgumentList)
    & $Executable @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable exited with code $LASTEXITCODE."
    }
}

function Invoke-Configure([string]$PresetName) {
    Invoke-Checked cmake (@('--preset', $PresetName) + $Arguments)
}

function Invoke-Build([string]$PresetName, [string]$Configuration) {
    if (-not (Test-Path (Join-Path (Get-BuildDir $PresetName) 'build.ninja'))) {
        Invoke-Configure $PresetName
    }
    [string[]]$buildArguments = @('--build', (Get-BuildDir $PresetName), '--config', $Configuration)
    if ($Target) {
        $buildArguments += @('--target', $Target)
    }
    Invoke-Checked cmake $buildArguments
}

function Invoke-Test([string]$PresetName, [string]$Configuration) {
    [string[]]$testArguments = @('--test-dir', (Get-BuildDir $PresetName), '-C', $Configuration, '--output-on-failure', '-j', '8')
    if ($Filter) {
        $testArguments += @('-R', $Filter)
    }
    if ($ExcludeLabel) {
        $testArguments += @('-LE', $ExcludeLabel)
    }
    Invoke-Checked ctest $testArguments
}

function Get-LlvmTool([string]$Name) {
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }
    $bundled = Join-Path (Find-VisualStudio) "VC\Tools\Llvm\x64\bin\$Name.exe"
    if (-not (Test-Path $bundled)) {
        throw "$Name was not found. Install the 'C++ Clang tools for Windows' Visual Studio component."
    }
    return $bundled
}

function Get-SourceFiles([string[]]$Directories) {
    $files = @()
    foreach ($directory in $Directories) {
        $path = Join-Path $repoRoot $directory
        if (Test-Path $path) {
            $files += Get-ChildItem -LiteralPath $path -Recurse -File |
                Where-Object { $_.Extension -in '.h', '.cpp', '.inl' } |
                ForEach-Object { $_.FullName }
        }
    }
    return $files
}

function Invoke-Format([switch]$CheckOnly) {
    $clangFormat = Get-LlvmTool 'clang-format'
    [string[]]$files = @(Get-SourceFiles $sourceDirectories)
    [string[]]$options = @('--style=file')
    $options += $(if ($CheckOnly) { @('--dry-run', '--Werror') } else { @('-i') })
    & $clangFormat @options @files
    if ($LASTEXITCODE -ne 0) {
        throw 'Formatting check failed. Run: scripts/dev.ps1 format'
    }
    Write-Host ("clang-format: {0} files {1}." -f $files.Count, $(if ($CheckOnly) { 'clean' } else { 'formatted' }))
}

function Invoke-Tidy {
    $clangTidy = Get-LlvmTool 'clang-tidy'
    $buildDir = Get-BuildDir 'windows-clang'
    if (-not (Test-Path (Join-Path $buildDir 'compile_commands.json'))) {
        Invoke-Configure 'windows-clang'
    }
    [string[]]$files = @(Get-SourceFiles $lintDirectories | Where-Object { $_.EndsWith('.cpp') })
    if ($Filter) {
        $files = @($files | Where-Object { $_ -match $Filter })
    }
    $failed = $false
    foreach ($file in $files) {
        & $clangTidy -p $buildDir --quiet $file
        if ($LASTEXITCODE -ne 0) {
            $failed = $true
        }
    }
    if ($failed) {
        throw 'clang-tidy reported problems.'
    }
    Write-Host ("clang-tidy: {0} files clean." -f $files.Count)
}

function Invoke-Verify {
    $steps = @(
        @{ Name = 'format-check'; Action = { Invoke-Format -CheckOnly } },
        @{ Name = 'msvc debug'; Action = { Invoke-Build 'windows' 'Debug'; Invoke-Test 'windows' 'Debug' } },
        @{ Name = 'msvc development'; Action = { Invoke-Build 'windows' 'RelWithDebInfo'; Invoke-Test 'windows' 'RelWithDebInfo' } },
        @{ Name = 'msvc release'; Action = { Invoke-Build 'windows' 'Release'; Invoke-Test 'windows' 'Release' } },
        @{ Name = 'clang-cl development'; Action = { Invoke-Build 'windows-clang' 'RelWithDebInfo'; Invoke-Test 'windows-clang' 'RelWithDebInfo' } },
        @{ Name = 'clang-tidy'; Action = { Invoke-Tidy } }
    )
    $results = @()
    foreach ($step in $steps) {
        try {
            & $step.Action
            $results += "PASS  $($step.Name)"
        }
        catch {
            $results += "FAIL  $($step.Name): $($_.Exception.Message)"
        }
    }
    Write-Host ''
    $results | ForEach-Object { Write-Host $_ }
    if ($results | Where-Object { $_.StartsWith('FAIL') }) {
        exit 1
    }
}

Import-BuildEnvironment

switch ($Command) {
    'configure' { Invoke-Configure $Preset }
    'build' { Invoke-Build $Preset $Config }
    'test' { Invoke-Test $Preset $Config }
    'format' { Invoke-Format }
    'format-check' { Invoke-Format -CheckOnly }
    'tidy' { Invoke-Tidy }
    'verify' { Invoke-Verify }
    'clean' {
        $buildDir = Get-BuildDir $Preset
        if (Test-Path $buildDir) {
            Remove-Item -LiteralPath $buildDir -Recurse -Force
        }
    }
}
