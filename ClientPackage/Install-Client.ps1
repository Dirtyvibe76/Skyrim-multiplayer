param(
    [Parameter(Mandatory = $false)]
    [string]$SkyrimPath
)

$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$dllSource = Join-Path $scriptRoot 'SkyrimMultiplayer.dll'

if (-not (Test-Path $dllSource)) {
    throw "SkyrimMultiplayer.dll was not found next to this installer."
}

if ([string]::IsNullOrWhiteSpace($SkyrimPath)) {
    $defaultPath = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition'
    $SkyrimPath = Read-Host "Enter Skyrim Special Edition folder [$defaultPath]"
    if ([string]::IsNullOrWhiteSpace($SkyrimPath)) {
        $SkyrimPath = $defaultPath
    }
}

$skyrimExe = Join-Path $SkyrimPath 'SkyrimSE.exe'
if (-not (Test-Path $skyrimExe)) {
    throw "SkyrimSE.exe was not found in '$SkyrimPath'."
}

$pluginDir = Join-Path $SkyrimPath 'Data\SKSE\Plugins'
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
$destination = Join-Path $pluginDir 'SkyrimMultiplayer.dll'
Copy-Item -Force $dllSource $destination

Write-Host ''
Write-Host 'SkyrimMultiplayer client installed:'
Write-Host "  $destination"
Write-Host ''
Write-Host 'Next:'
Write-Host '  1. Run Connect-To-SkyrimMP.ps1 and enter the server IPv4 address.'
Write-Host '  2. Leave the relay window open.'
Write-Host '  3. Launch Skyrim through SKSE.'
