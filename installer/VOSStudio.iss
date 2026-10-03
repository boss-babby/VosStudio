; Inno Setup script for VOSStudio Native (https://jrsoftware.org/isinfo.php).
; Build:  ISCC.exe /DAppVersion=1.19.0 installer\VOSStudio.iss     (from the repository root, after `make`)
; Produces installer\Output\VOSStudio-<version>-setup.exe: per-user install (no admin rights), Start menu entry,
; optional desktop icon, file associations for .vosproj projects, clean uninstall.
#ifndef AppVersion
  #define AppVersion "1.19.0"
#endif
#define AppName "VOSStudio"
#define AppExe "VOSStudio.exe"

[Setup]
AppId={{7D0C1F6E-2C7B-4B7A-9D4B-3F1A9C2E5B01}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
AppPublisherURL=https://github.com/boss-babby/VosStudio
AppSupportURL=https://github.com/boss-babby/VosStudio/issues
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=Output
OutputBaseFilename={#AppName}-{#AppVersion}-setup
SetupIconFile=..\res\vosstudio.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
ChangesAssociations=yes
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assoc"; Description: "Open .vosproj project files with {#AppName}"; GroupDescription: "File associations:"

[Files]
Source: "..\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\samples\*"; DestDir: "{app}\samples"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.vosproj"; ValueType: string; ValueName: ""; ValueData: "VOSStudio.Project"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\VOSStudio.Project"; ValueType: string; ValueName: ""; ValueData: "VOSStudio project"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\VOSStudio.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\VOSStudio.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: assoc

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
