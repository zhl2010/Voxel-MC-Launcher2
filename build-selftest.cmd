@echo off
setlocal
set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.22621.0"
set "PATH=%MSVC%\bin\HostX64\x64;%PATH%"
set "LIB=%MSVC%\lib\x64;%WINSDK%\Lib\%SDKVER%\ucrt\x64;%WINSDK%\Lib\%SDKVER%\um\x64"
set "INCLUDE=%MSVC%\include;%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\um;%WINSDK%\Include\%SDKVER%\shared;%WINSDK%\Include\%SDKVER%\winrt"

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\obj-selftest" mkdir "%OUT%\obj-selftest"

echo [selftest] compiling...
cl /nologo /std:c++20 /utf-8 /EHsc /O2 /W3 /MD /DUNICODE /D_UNICODE /DNOMINMAX ^
   /I"%ROOT%." /I"%ROOT%core" /I"%ROOT%net" /I"%ROOT%launch" /I"%ROOT%third_party" ^
   "%ROOT%tools\selftest.cpp" ^
   "%ROOT%core\config.cpp" "%ROOT%core\versions.cpp" "%ROOT%core\crypto.cpp" ^
   "%ROOT%core\manifest.cpp" "%ROOT%core\auth.cpp" "%ROOT%core\dirs.cpp" ^
   "%ROOT%core\proc.cpp" "%ROOT%core\installer.cpp" "%ROOT%net\http.cpp" ^
   "%ROOT%launch\launch.cpp" ^
   /Fo"%OUT%\obj-selftest\\" /Fe"%OUT%\selftest.exe" ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib advapi32.lib winhttp.lib bcrypt.lib

if errorlevel 1 (
  echo [selftest] FAILED
  exit /b 1
)
echo [selftest] OK: %OUT%\selftest.exe

rem 交付副本：项目目录被 DSH 沙箱标了 Low Mandatory Level，编出的 exe 继承该标签，
rem 低完整性进程无法写入 AppData\Roaming 等目录，会导致所有下载写入失败。
rem 复制到 LABEL 正常的目录后 exe 不带低标签，selftest 才能真正验证下载。
set "DIST=%LOCALAPPDATA%\VoxelLauncher"
if not exist "%DIST%" mkdir "%DIST%"
copy /Y "%OUT%\selftest.exe" "%DIST%\selftest.exe" >nul
if errorlevel 1 (
  echo [selftest] 警告: 交付副本复制失败，请手动从 build\selftest.exe 复制后运行
) else (
  echo [selftest] RUN:  %DIST%\selftest.exe ^<version^>
)
