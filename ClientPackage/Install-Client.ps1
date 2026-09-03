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

$launcherConfig = Join-Path $scriptRoot 'SkyrimMP-Launcher.ini'
if (-not (Test-Path $launcherConfig)) {
    @(
        "SkyrimPath=$SkyrimPath"
        'ServerAddress=127.0.0.1'
    ) | Set-Content -Path $launcherConfig -Encoding UTF8
}

Write-Host ''
Write-Host 'SkyrimMultiplayer client installed:'
Write-Host "  $destination"
Write-Host ''
Write-Host 'Use the included "SkyrimMP Launcher.cmd" from now on.'
Write-Host '  Play Single Player  -> disables only SkyrimMultiplayer.dll and starts SKSE.'
Write-Host '  Play Multiplayer    -> enables SkyrimMultiplayer.dll, starts the relay, then starts SKSE.'
