@echo off
setlocal EnableExtensions
set "src=%~dp0"
set "fps=%~dp0..\..\scripts\windows\fps"
set "vs="
set "dest=%~1"
if not defined dest set "dest=%LOCALAPPDATA%\WT_USB_Display\build"
for %%D in ("%dest%") do set "dest=%%~fD"
if not exist "%dest%\" mkdir "%dest%" || exit /b 1
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    for /f "usebackq delims=" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vs=%%V"
)
if defined vs call "%vs%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo Start this script in the x64 Native Tools Command Prompt. 1>&2
    exit /b 1
)
set "csc=%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
set "wpf=%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\WPF"
pushd "%dest%" || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /LD "%src%GpuCapture.cpp" "%src%Jpeg444.cpp" /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib ole32.lib windowscodecs.lib shlwapi.lib /OUT:GpuCapture.dll || goto failed
"%csc%" /nologo /codepage:65001 /optimize+ /platform:x64 /target:exe /out:P4Desktop.exe /r:System.Drawing.dll /r:System.Windows.Forms.dll "%src%P4Desktop.cs" "%src%ChromaPreference.cs" || goto failed
"%csc%" /nologo /codepage:65001 /optimize+ /platform:x64 /target:winexe /out:"Wireless-tag USB Display.exe" /win32icon:"%src%display.ico" ^
    /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Xaml.dll /lib:"%wpf%" ^
    /r:WindowsBase.dll /r:PresentationCore.dll /r:PresentationFramework.dll /r:WindowsFormsIntegration.dll ^
    /resource:"%src%DisplayView.xaml",DisplayView.xaml /resource:"%src%DisplayStyles.xaml",DisplayStyles.xaml /resource:"%src%DisplayAbout.xaml",DisplayAbout.xaml /resource:"%src%display.ico",display.ico ^
    "%src%DisplayApp.cs" "%src%DisplayMenus.cs" "%src%DisplayView.cs" "%src%ChromaPreference.cs" || goto failed
if /i "%~2"=="app" goto success
cl /nologo /O2 /MT /EHsc "%fps%\Camera.cpp" /link mfplat.lib mfreadwrite.lib mf.lib mfuuid.lib ole32.lib /OUT:Camera.exe || goto failed
"%csc%" /nologo /codepage:65001 /optimize+ /platform:x64 /target:winexe /out:MotionMeter.exe /r:System.Drawing.dll /r:System.Windows.Forms.dll "%fps%\MotionMeter.cs" || goto failed
"%csc%" /nologo /codepage:65001 /optimize+ /platform:x64 /target:exe /out:RateChart.exe /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Windows.Forms.DataVisualization.dll "%fps%\RateChart.cs" || goto failed
:success
popd
echo Binaries in "%dest%"
exit /b 0
:failed
popd
exit /b 1
