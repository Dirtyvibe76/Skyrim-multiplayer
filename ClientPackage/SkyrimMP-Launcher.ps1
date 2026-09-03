Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$configPath = Join-Path $scriptRoot 'SkyrimMP-Launcher.ini'
$relayScript = Join-Path $scriptRoot 'Connect-To-SkyrimMP.ps1'

function Read-Config {
    $cfg = @{
        SkyrimPath = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition'
        ServerAddress = '127.0.0.1'
    }
    if (Test-Path $configPath) {
        foreach ($line in Get-Content $configPath) {
            if ($line -match '^\s*([^#;][^=]*)=(.*)$') {
                $cfg[$matches[1].Trim()] = $matches[2].Trim()
            }
        }
    }
    return $cfg
}

function Write-Config([string]$SkyrimPath, [string]$ServerAddress) {
    @(
        "SkyrimPath=$SkyrimPath"
        "ServerAddress=$ServerAddress"
    ) | Set-Content -Path $configPath -Encoding UTF8
}

function Get-PluginPaths([string]$SkyrimPath) {
    $pluginDir = Join-Path $SkyrimPath 'Data\SKSE\Plugins'
    return @{
        Enabled = Join-Path $pluginDir 'SkyrimMultiplayer.dll'
        Disabled = Join-Path $pluginDir 'SkyrimMultiplayer.dll.disabled'
    }
}

function Set-MultiplayerEnabled([string]$SkyrimPath, [bool]$Enabled) {
    $paths = Get-PluginPaths $SkyrimPath
    if ($Enabled) {
        if (-not (Test-Path $paths.Enabled) -and (Test-Path $paths.Disabled)) {
            Move-Item -Force $paths.Disabled $paths.Enabled
        }
        if (-not (Test-Path $paths.Enabled)) {
            $source = Join-Path $scriptRoot 'SkyrimMultiplayer.dll'
            if (-not (Test-Path $source)) { throw 'SkyrimMultiplayer.dll is missing from the launcher package.' }
            New-Item -ItemType Directory -Force -Path (Split-Path $paths.Enabled -Parent) | Out-Null
            Copy-Item -Force $source $paths.Enabled
        }
    } else {
        if (Test-Path $paths.Enabled) {
            Move-Item -Force $paths.Enabled $paths.Disabled
        }
    }
}

function Validate-SkyrimPath([string]$SkyrimPath) {
    if (-not (Test-Path (Join-Path $SkyrimPath 'SkyrimSE.exe'))) { throw "SkyrimSE.exe not found in '$SkyrimPath'." }
    if (-not (Test-Path (Join-Path $SkyrimPath 'skse64_loader.exe'))) { throw "skse64_loader.exe not found in '$SkyrimPath'. Install SKSE64 first." }
}

function Launch-Skyrim([string]$SkyrimPath) {
    Start-Process -FilePath (Join-Path $SkyrimPath 'skse64_loader.exe') -WorkingDirectory $SkyrimPath
}

function Stop-OldRelay {
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*Connect-To-SkyrimMP.ps1*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

$config = Read-Config

$form = New-Object System.Windows.Forms.Form
$form.Text = 'SkyrimMP Launcher'
$form.Size = New-Object System.Drawing.Size(560, 330)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false

$title = New-Object System.Windows.Forms.Label
$title.Text = 'Skyrim Multiplayer'
$title.Font = New-Object System.Drawing.Font('Segoe UI', 20, [System.Drawing.FontStyle]::Bold)
$title.AutoSize = $true
$title.Location = New-Object System.Drawing.Point(20, 18)
$form.Controls.Add($title)

$pathLabel = New-Object System.Windows.Forms.Label
$pathLabel.Text = 'Skyrim folder'
$pathLabel.Location = New-Object System.Drawing.Point(22, 72)
$pathLabel.AutoSize = $true
$form.Controls.Add($pathLabel)

$pathBox = New-Object System.Windows.Forms.TextBox
$pathBox.Text = $config.SkyrimPath
$pathBox.Location = New-Object System.Drawing.Point(22, 94)
$pathBox.Size = New-Object System.Drawing.Size(430, 24)
$form.Controls.Add($pathBox)

$browse = New-Object System.Windows.Forms.Button
$browse.Text = 'Browse'
$browse.Location = New-Object System.Drawing.Point(462, 92)
$browse.Size = New-Object System.Drawing.Size(72, 28)
$browse.Add_Click({
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Select the Skyrim Special Edition folder'
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $pathBox.Text = $dlg.SelectedPath }
})
$form.Controls.Add($browse)

$serverLabel = New-Object System.Windows.Forms.Label
$serverLabel.Text = 'Multiplayer server IPv4 / hostname'
$serverLabel.Location = New-Object System.Drawing.Point(22, 132)
$serverLabel.AutoSize = $true
$form.Controls.Add($serverLabel)

$serverBox = New-Object System.Windows.Forms.TextBox
$serverBox.Text = $config.ServerAddress
$serverBox.Location = New-Object System.Drawing.Point(22, 154)
$serverBox.Size = New-Object System.Drawing.Size(260, 24)
$form.Controls.Add($serverBox)

$single = New-Object System.Windows.Forms.Button
$single.Text = 'Play Single Player'
$single.Location = New-Object System.Drawing.Point(22, 205)
$single.Size = New-Object System.Drawing.Size(240, 55)
$single.Add_Click({
    try {
        Validate-SkyrimPath $pathBox.Text
        Write-Config $pathBox.Text $serverBox.Text
        Stop-OldRelay
        Set-MultiplayerEnabled $pathBox.Text $false
        Launch-Skyrim $pathBox.Text
        $form.Close()
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'SkyrimMP Launcher', 'OK', 'Error') | Out-Null
    }
})
$form.Controls.Add($single)

$multi = New-Object System.Windows.Forms.Button
$multi.Text = 'Play Multiplayer'
$multi.Location = New-Object System.Drawing.Point(294, 205)
$multi.Size = New-Object System.Drawing.Size(240, 55)
$multi.Add_Click({
    try {
        Validate-SkyrimPath $pathBox.Text
        if ([string]::IsNullOrWhiteSpace($serverBox.Text)) { throw 'Enter the multiplayer server IPv4 address or hostname.' }
        if (-not (Test-Path $relayScript)) { throw 'Connect-To-SkyrimMP.ps1 is missing from the launcher package.' }
        Write-Config $pathBox.Text $serverBox.Text
        Set-MultiplayerEnabled $pathBox.Text $true
        Stop-OldRelay
        Start-Process powershell.exe -ArgumentList @('-ExecutionPolicy','Bypass','-File',('"' + $relayScript + '"'),'-ServerAddress',('"' + $serverBox.Text + '"'))
        Start-Sleep -Milliseconds 500
        Launch-Skyrim $pathBox.Text
        $form.Close()
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'SkyrimMP Launcher', 'OK', 'Error') | Out-Null
    }
})
$form.Controls.Add($multi)

$note = New-Object System.Windows.Forms.Label
$note.Text = 'Single Player disables only SkyrimMultiplayer.dll. Multiplayer re-enables it and starts the UDP relay.'
$note.Location = New-Object System.Drawing.Point(22, 272)
$note.Size = New-Object System.Drawing.Size(510, 34)
$form.Controls.Add($note)

[void]$form.ShowDialog()
