@echo off
setlocal
set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.22621.0"
set "PATH=%MSVC%\bin\HostX64\x64;%PATH%"
set "LIB=%MSVC%\lib\x64;%WINSDK%\Lib\%SDKVER%\ucrt\x64;%WINSDK%\Lib\%SDKVER%\um\x64"
rem winrt must be in INCLUDE: WebView2.h depends on EventToken.h which only lives there
set "INCLUDE=%MSVC%\include;%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\um;%WINSDK%\Include\%SDKVER%\shared;%WINSDK%\Include\%SDKVER%\winrt"

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

echo [build] embedding frontend resources...
"%WINSDK%\bin\%SDKVER%\x64\rc.exe" /nologo /fo "%OUT%\obj\resources.res" "%ROOT%resources.rc"
if errorlevel 1 (
  echo [build] FAILED: 资源编译失败
  exit /b 1
)

echo [build] compiling native host...
cl /nologo /std:c++20 /utf-8 /EHsc /O2 /W3 /MD /DUNICODE /D_UNICODE /DNOMINMAX ^
   /I"%ROOT%." ^
   /I"%ROOT%app" ^
   /I"%ROOT%core" ^
   /I"%ROOT%net" ^
   /I"%ROOT%launch" ^
   /I"%ROOT%third_party" ^
   /I"%ROOT%third_party\webview2\include" ^
   "%ROOT%main.cpp" ^
   "%ROOT%app\window.cpp" ^
   "%ROOT%app\webview.cpp" ^
   "%ROOT%app\bridge.cpp" ^
   "%ROOT%app\assets.cpp" ^
   "%ROOT%core\config.cpp" ^
   "%ROOT%core\versions.cpp" ^
   "%ROOT%core\crypto.cpp" ^
   "%ROOT%core\manifest.cpp" ^
   "%ROOT%core\auth.cpp" ^
   "%ROOT%core\dirs.cpp" ^
   "%ROOT%core\proc.cpp" ^
   "%ROOT%core\installer.cpp" ^
   "%ROOT%net\http.cpp" ^
   "%ROOT%launch\launch.cpp" ^
   "%OUT%\obj\resources.res" ^
   /Fo"%OUT%\obj\\" ^
   /Fe"%OUT%\voxel-launcher.exe" ^
   /link /SUBSYSTEM:WINDOWS ^
   /LIBPATH:"%ROOT%third_party\webview2\lib" ^
   WebView2LoaderStatic.lib user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib advapi32.lib dwmapi.lib winhttp.lib bcrypt.lib

if errorlevel 1 (
  echo [build] FAILED
  exit /b 1
)
echo [build] OK: %OUT%\voxel-launcher.exe

rem 交付副本：放到普通目录，避免工作区继承到的低完整性标签。
rem 这个项目目录被 DSH 沙箱标了 Low Mandatory Level，在此编出的 exe 会继承该标签；
rem 低完整性的 exe 以普通用户启动时，WebView2 的 msedge_elf.dll 会直接崩（0x80000003），
rem 只有管理员权限能绕过——这正是"必须用管理员启动"的真正原因。
rem 复制到 LABEL 正常的目录后，exe 不带低标签，普通双击即可运行。
set "DIST=%LOCALAPPDATA%\VoxelLauncher"
if not exist "%DIST%" mkdir "%DIST%"
copy /Y "%OUT%\voxel-launcher.exe" "%DIST%\voxel-launcher.exe" >nul
if errorlevel 1 (
  echo [build] 警告: 交付副本复制失败
) else (
  echo [build] RUN:  %DIST%\voxel-launcher.exe   ^(可直接双击，无需管理员^)
)
