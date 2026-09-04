#define MyAppName "SkyrimMP"
#define MyAppVersion "0.1.0-alpha.1"
#define MyAppPublisher "SkyrimMP"
#define MyAppExeName "SkyrimMPLauncher.exe"

[Setup]
AppId={{8D1241F4-4D36-4FD6-B514-7C9D0D9A7F2B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\SkyrimMP
DefaultGroupName=SkyrimMP
OutputDir=..\release
OutputBaseFilename=SkyrimMP-0.1.0-alpha.1-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes
RestartApplications=no

[Files]
Source: "..\artifact\SkyrimMultiplayer-Client\SkyrimMPLauncher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifact\SkyrimMultiplayer-Client\SkyrimMultiplayer.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifact\SkyrimMultiplayer-Client\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifact\SkyrimMultiplayer-Client\SHA256.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifact\SkyrimMultiplayer-Client\PLAYABLE_ALPHA_TEST.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\SkyrimMP Launcher"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\SkyrimMP Launcher"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch SkyrimMP"; Flags: nowait postinstall skipifsilent
