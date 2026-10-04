
#define Title "CS2 - Sound Player"
#define AppPath "C:\Users\Administrator\Desktop\Code\cssound"
#define BasePath "C:\Users\Administrator\Desktop\Code\cssound\output\portable"
#define OutputPath "C:\Program Files (x86)\cs2-sound-player"

[Setup]
AppName={#Title}
AppVersion=1.0
AppPublisher=NxRoot
VersionInfoDescription={#Title}
DefaultDirName={#OutputPath}
OutputDir=userdocs:Inno Setup Examples Output
SolidCompression=yes
WizardStyle=classic
Uninstallable=yes
DirExistsWarning=no
DisableWelcomePage=yes
DisableProgramGroupPage=yes
SetupIconFile={#AppPath}\icon.ico
UninstallDisplayIcon={#AppPath}\icon.ico
OutputBaseFilename={#Title} Setup

[Files]
Source: "{#BasePath}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#Title}"; Filename: "{app}\CS2SP.exe"
Name: "{autodesktop}\{#Title}"; Filename: "{app}\CS2SP.exe"