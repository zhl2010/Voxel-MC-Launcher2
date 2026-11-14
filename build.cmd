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
