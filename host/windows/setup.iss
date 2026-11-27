; Built twice by package.py. Only the small driver installer needs elevation.
#define EnglishName GetStringFileInfo(AppExe, PRODUCT_NAME)
#define ChineseName "Wireless-tag USB 副屏"
#define AppFilename ExtractFileName(AppExe)
#define LegacyExe "WTUSBDisplay.exe"
#define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"

[Setup]
AppVersion={#GetFileProductVersion(AppExe)}
AppPublisher={#GetFileCompany(AppExe)}
AppCopyright={#GetFileCopyright(AppExe)}
VersionInfoVersion={#GetVersionNumbersString(AppExe)}
VersionInfoProductName={#EnglishName}
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19041
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableFinishedPage=yes
ShowLanguageDialog=no
SetupIconFile=display.ico
#ifdef SignArtifacts
SignTool=wt
#endif
#ifdef DriverOnly
AppName={cm:DriverTitle}
VersionInfoDescription={#EnglishName} Driver Setup
AppId=WTUSBDisplay.DriverSetup
PrivilegesRequired=admin
CreateAppDir=no
Uninstallable=no
SetupMutex=Global\WTUSBDisplay.DriverSetup
OutputBaseFilename={#PackagePrefix}-Driver
DisableReadyPage=yes
#else
AppName={cm:ProductTitle}
VersionInfoDescription={#EnglishName} Setup
AppId=WTUSBDisplay
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#EnglishName}
UninstallDisplayIcon={app}\{#AppFilename}
OutputBaseFilename={#PackagePrefix}-Setup
RestartApplications=no
#ifdef SignArtifacts
SignedUninstaller=yes
#endif
#endif

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "zh"; MessagesFile: "{#ChineseMessages}"

[CustomMessages]
en.ProductTitle={#EnglishName}
zh.ProductTitle={#ChineseName}
en.DriverTitle={#EnglishName} Driver
zh.DriverTitle={#ChineseName}显示驱动
en.LanguageCode=en
zh.LanguageCode=zh-CN
en.DriverQueryFailed=Windows could not read the display driver status (%1).
zh.DriverQueryFailed=无法读取 Windows 显示驱动状态（%1）。
en.DriverDisabled=Windows already has this driver. Enable USB Mobile Monitor Virtual Display in Device Manager, or restart and try again.
zh.DriverDisabled=Windows 已登记此驱动。请在设备管理器中启用 USB Mobile Monitor Virtual Display，或重启后重试。
en.DriverLaunchFailed=Could not start the display driver installer.
zh.DriverLaunchFailed=无法启动显示驱动安装程序。
en.DriverEnableFailed=Windows could not enable the display driver (installer code %1). Restart and try again, or contact your administrator.
zh.DriverEnableFailed=Windows 尚未启用显示驱动（安装返回 %1）。请重启后重试，或联系电脑管理员。
en.ExitToInstall=Quit {#EnglishName}, then run the installer again. In the current version, choose More → Exit.
zh.ExitToInstall=请退出 {#ChineseName} 后重试安装。当前版本可选择“更多 → 退出”。
en.AdminRequired=Administrator approval is required to install the display driver for the first time.
zh.AdminRequired=首次安装显示驱动需要管理员许可。
en.DriverNotReady=The display driver is unavailable (%1). Check administrator approval and USB Mobile Monitor Virtual Display in Device Manager, then try again.
zh.DriverNotReady=显示驱动尚未就绪（%1）。请确认管理员许可，检查设备管理器中的 USB Mobile Monitor Virtual Display，然后重试。
en.ReadyText=Choose Install to open the app. Connect the HUSB port of your display to your PC to get started.%n%nWindows will ask for administrator approval the first time it installs the Amyuni virtual display driver. The driver is provided by Amyuni Technologies Inc. Its license mentions that it may display an advertising page.
zh.ReadyText=点击“安装”后，应用会自动打开。将已烧录的副屏通过 HUSB 接到电脑即可使用。%n%n首次安装 Amyuni 虚拟显示驱动时，Windows 会请求管理员许可。驱动由 Amyuni Technologies Inc. 提供，其许可说明中提及可能显示广告页。
en.ExitToUninstall=Open {#EnglishName}, choose More → Exit, then try uninstalling again.
zh.ExitToUninstall=请打开 {#ChineseName}，选择“更多 → 退出”，再重试卸载。

[Files]
#ifdef DriverOnly
Source: "{#DriverFiles}\*"; DestDir: "{tmp}\WTDisplayDriver"; Flags: dontcopy recursesubdirs
#else
Source: "{#DriverInstaller}"; Flags: dontcopy
#ifdef SignArtifacts
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion signonce
Source: "{#Payload}\P4Desktop.exe"; DestDir: "{app}"; Flags: ignoreversion signonce
Source: "{#Payload}\GpuCapture.dll"; DestDir: "{app}"; Flags: ignoreversion signonce
Source: "{#Payload}\*"; Excludes: "{#AppFilename},P4Desktop.exe,GpuCapture.dll"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
#else
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
#endif
; Keep existing pinned or user-created shortcuts working after an upgrade.
Source: "{#AppExe}"; DestDir: "{app}"; DestName: "{#LegacyExe}"; Flags: ignoreversion; Check: FileExists(ExpandConstant('{app}\{#LegacyExe}'))

[InstallDelete]
Type: files; Name: "{userprograms}\WT USB 副屏.lnk"
Type: files; Name: "{userprograms}\WT USB Display.lnk"
Type: files; Name: "{userprograms}\{#ChineseName}.lnk"; Languages: en
Type: files; Name: "{userprograms}\{#EnglishName}.lnk"; Languages: zh

[Icons]
Name: "{userprograms}\{cm:ProductTitle}"; Filename: "{app}\{#AppFilename}"

[Registry]
Root: HKCU; Subkey: "{#RunKey}"; ValueName: "WTUSBDisplay"; Flags: dontcreatekey uninsdeletevalue
Root: HKCU; Subkey: "{#RunKey}"; ValueName: "WTUSBDisplay"; ValueType: string; ValueData: """{app}\{#AppFilename}"" --tray"; Flags: dontcreatekey; Check: HasLegacyStartup
Root: HKCU; Subkey: "Software\WTUSBDisplay"; ValueType: string; ValueName: "Language"; ValueData: "{cm:LanguageCode}"; Flags: createvalueifdoesntexist uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\WTUSBDisplay"; ValueName: "ChromaSampling"; Flags: dontcreatekey uninsdeletevalue

[Run]
Filename: "{app}\{#AppFilename}"; Flags: nowait skipifsilent runasoriginaluser

[UninstallDelete]
Type: files; Name: "{app}\__pycache__\*.pyc"
Type: dirifempty; Name: "{app}\__pycache__"
Type: files; Name: "{localappdata}\WTUSBDisplay\session\*.json"
Type: files; Name: "{localappdata}\WTUSBDisplay\session\*.pending"
Type: files; Name: "{localappdata}\WTUSBDisplay\session\*.log"
Type: files; Name: "{localappdata}\WTUSBDisplay\session\stop.request"
Type: dirifempty; Name: "{localappdata}\WTUSBDisplay\session"
Type: dirifempty; Name: "{localappdata}\WTUSBDisplay"
#endif

[Code]
function CM_Get_Device_Interface_List_SizeW(var Size: Cardinal; var Guid: TGUID;
  DeviceID: LongWord; Flags: Cardinal): Cardinal;
  external 'CM_Get_Device_Interface_List_SizeW@cfgmgr32.dll stdcall';

function HasDriverInterface(Flags: Cardinal): Boolean;
var
  Guid: TGUID;
  Size, Code: Cardinal;
begin
  Guid := StringToGUID('{B5FFD75F-DA40-4353-8FF8-B6DAF6F1D8CA}');
  Code := CM_Get_Device_Interface_List_SizeW(Size, Guid, 0, Flags);
  if Code <> 0 then RaiseException(FmtMessage(CustomMessage('DriverQueryFailed'), [IntToStr(Code)]));
  Result := Size > 1;
end;

#ifdef DriverOnly
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Folder: String;
  Code, I: Integer;
begin
  Result := '';
  if HasDriverInterface(0) then Exit;
  if HasDriverInterface(1) then begin
    Result := CustomMessage('DriverDisabled');
    Exit;
  end;
  ExtractTemporaryFiles('{tmp}\WTDisplayDriver\*');
  Folder := ExpandConstant('{tmp}\WTDisplayDriver');
  if not Exec(Folder + '\deviceinstaller64.exe', 'install usbmmIdd.inf usbmmidd',
              Folder, SW_HIDE, ewWaitUntilTerminated, Code) then begin
    Result := CustomMessage('DriverLaunchFailed') + ' ' + SysErrorMessage(Code);
    Exit;
  end;
  Log(Format('Amyuni device installer exit code %d', [Code]));
  for I := 1 to 60 do begin
    if HasDriverInterface(0) then Exit;
    Sleep(250);
  end;
  Result := FmtMessage(CustomMessage('DriverEnableFailed'), [IntToStr(Code)]);
end;
#else
function HasLegacyStartup: Boolean;
var
  Command: String;
begin
  Result := RegQueryStringValue(HKCU, '{#RunKey}', 'WTUSBDisplay', Command) and
    (CompareText(Command, ExpandConstant('"{app}\{#LegacyExe}" --tray')) = 0);
end;

function OpenEventW(Access: Cardinal; Inherit: Boolean; Name: String): THandle;
  external 'OpenEventW@kernel32.dll stdcall';
function SetEvent(Handle: THandle): Boolean; external 'SetEvent@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean; external 'CloseHandle@kernel32.dll stdcall';

function StopApp: Boolean;
var
  Event: THandle;
  I: Integer;
begin
  Result := True;
  if not CheckForMutexes('Local\WTUSBDisplay.App') then Exit;
  Event := OpenEventW(2, False, 'Local\WTUSBDisplay.Quit');
  if Event <> 0 then begin
    SetEvent(Event);
    CloseHandle(Event);
  end;
  for I := 1 to 80 do begin
    if not CheckForMutexes('Local\WTUSBDisplay.App') then Exit;
    Sleep(250);
  end;
  Result := False;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  if not StopApp then begin
    Result := CustomMessage('ExitToInstall');
    Exit;
  end;
  if HasDriverInterface(0) then Exit;
  ExtractTemporaryFile('{#PackagePrefix}-Driver.exe');
  if not ShellExec('runas', ExpandConstant('{tmp}\{#PackagePrefix}-Driver.exe'),
      '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LANG=' + ActiveLanguage, '', SW_SHOW, ewWaitUntilTerminated, Code) then begin
    Result := CustomMessage('AdminRequired') + ' ' + SysErrorMessage(Code);
    Exit;
  end;
  if not HasDriverInterface(0) then
    Result := FmtMessage(CustomMessage('DriverNotReady'), [IntToStr(Code)]);
end;

function UpdateReadyMemo(Space, NewLine, UserInfo, DirInfo, TypeInfo, ComponentsInfo,
  GroupInfo, TasksInfo: String): String;
begin
  Result := CustomMessage('ReadyText');
end;

function InitializeUninstall: Boolean;
begin
  Result := StopApp;
  if not Result then SuppressibleMsgBox(CustomMessage('ExitToUninstall'), mbError, MB_OK, IDOK);
end;
#endif
