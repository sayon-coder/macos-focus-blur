$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sln = Join-Path $root 'FocusGlass.sln'

# Locate the VS 2022 Build Tools installation.
$programFilesRoots = @(
    [Environment]::GetFolderPath('ProgramFiles'),
    [Environment]::GetFolderPath('ProgramFilesX86')
) | Where-Object { $_ } | Select-Object -Unique

$vsRoots = foreach ($programFilesRoot in $programFilesRoots) {
    foreach ($edition in @('BuildTools', 'Community', 'Professional', 'Enterprise')) {
        $candidate = Join-Path $programFilesRoot "Microsoft Visual Studio\2022\$edition"
        if (Test-Path $candidate) { $candidate }
    }
}

$msbuild = $null
$selectedVs = $null
foreach ($vsRoot in $vsRoots) {
    $candidate = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
    $msvcRoot = Join-Path $vsRoot 'VC\Tools\MSVC'
    $hasV143 = $false
    if (Test-Path $msvcRoot) {
        $hasV143 = [bool](Get-ChildItem $msvcRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^14\.(3|4)\d' } |
            Where-Object { Test-Path (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe') } |
            Select-Object -First 1)
    }
    if ((Test-Path $candidate) -and $hasV143) {
        $msbuild = $candidate
        $selectedVs = $vsRoot
        break
    }
}

if (-not $msbuild) {
    $command = Get-Command 'MSBuild.exe' -ErrorAction SilentlyContinue
    if ($command) { $msbuild = $command.Source }
}
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    throw 'MSBuild.exe was not found. Open Developer PowerShell for VS 2022 or repair the Build Tools installation.'
}

# Read the Windows SDK root from the registry.
$sdkRegistry = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
$sdkRoot = $sdkRegistry.KitsRoot10
if (-not $sdkRoot) {
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\'
}
$sdkInclude = Join-Path $sdkRoot 'Include'
if (-not (Test-Path $sdkInclude)) {
    throw "Windows SDK include directory was not found: $sdkInclude"
}

$sdk = Get-ChildItem $sdkInclude -Directory -ErrorAction Stop |
    Where-Object {
        try {
            ([version]$_.Name -ge [version]'10.0.20348.0') -and
            (Test-Path (Join-Path $_.FullName 'cppwinrt\winrt\base.h'))
        }
        catch { $false }
    } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1

if (-not $sdk) {
    throw 'No Windows SDK 10.0.20348.0+ with C++/WinRT projection headers was found. Install a Windows 10/11 SDK from Visual Studio Installer.'
}

Write-Host "Using MSBuild: $msbuild" -ForegroundColor Cyan
if ($selectedVs) { Write-Host "Using Visual Studio: $selectedVs" -ForegroundColor Cyan }
Write-Host "Using Windows SDK: $($sdk.Name)" -ForegroundColor Cyan
Write-Host 'Building FocusGlass (Release | x64). No NuGet restore is required; the SDK provides the C++/WinRT headers used by this project.' -ForegroundColor Cyan

& $msbuild $sln /m /t:Build /p:Configuration=Release /p:Platform=x64 "/p:WindowsTargetPlatformVersion=$($sdk.Name)" /verbosity:minimal
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE. Review the compiler output above."
}

$exe = Join-Path $root 'artifacts\x64\Release\FocusGlass.exe'
if (-not (Test-Path $exe)) {
    throw "Build reported success, but the expected executable was not found: $exe"
}

Write-Host "Build succeeded: $exe" -ForegroundColor Green
Write-Host 'Before testing, read README.md. Ctrl+Alt+B pauses; Ctrl+Alt+Q exits.' -ForegroundColor Yellow
