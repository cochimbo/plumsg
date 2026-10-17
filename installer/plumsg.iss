; Instalador de Windows para el emulador de Mega Drive / Genesis (plumsg).
; Se compila con Inno Setup 6:
;   iscc /DAppVersion=0.1.0 /DSourceDir=..\dist installer\plumsg.iss
; SourceDir debe contener el ejecutable (AppExeName) y SDL2.dll.

#ifndef AppName
  #define AppName "plumsg"
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef AppExeName
  #define AppExeName "plumsg.exe"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist-installer"
#endif
#define AppPublisher "cochimbo"
#define AppURL "https://github.com/cochimbo/plumsg"

[Setup]
; AppId identifica la instalación para actualizaciones y desinstalación: no cambiar.
AppId={{6378174E-7495-4B78-9F34-784944581E50}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExeName}
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; La marquesina en la columna izquierda de las páginas de bienvenida y final (100% y 200%).
WizardImageFile=wizard.bmp,wizard-2x.bmp
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Permite instalar sin permisos de administrador (en el perfil del usuario).
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
spanish.AssocMD=Añadir {#AppName} a "Abrir con" para las ROMs de Mega Drive (.md, .gen, .smd, .bin, .zip)
english.AssocMD=Add {#AppName} to "Open with" for Mega Drive ROMs (.md, .gen, .smd, .bin, .zip)
spanish.MDFileDesc=ROM de Mega Drive
english.MDFileDesc=Mega Drive ROM

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assocmd"; Description: "{cm:AssocMD}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\SDL2.dll"; DestDir: "{app}"; Flags: ignoreversion
; Cualquier otra DLL o recurso que acompañe al ejecutable.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "{#AppExeName},SDL2.dll,*.pdb,*.ilk,*.lib,*.exp"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
; Las extensiones solo se añaden a "Abrir con", sin quitar el programa que las abre por
; defecto (.md también son documentos Markdown, .bin y .zip muchas otras cosas). Se
; escriben en HKCU o HKLM según el modo de instalación.
Root: HKA; Subkey: "Software\Classes\.md\OpenWithProgids"; ValueType: string; ValueName: "plumsg.mdrom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.gen\OpenWithProgids"; ValueType: string; ValueName: "plumsg.mdrom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.smd\OpenWithProgids"; ValueType: string; ValueName: "plumsg.mdrom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.bin\OpenWithProgids"; ValueType: string; ValueName: "plumsg.mdrom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.zip\OpenWithProgids"; ValueType: string; ValueName: "plumsg.mdrom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\plumsg.mdrom"; ValueType: string; ValueName: ""; ValueData: "{cm:MDFileDesc}"; Flags: uninsdeletekey; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\plumsg.mdrom\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExeName},0"; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\plumsg.mdrom\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assocmd

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
