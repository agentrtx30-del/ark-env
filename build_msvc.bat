@echo off
setlocal EnableExtensions
rem ============================================================================
rem  ark-env build script (MSVC x64). Single-line constructs + CRLF endings.
rem  Finds vcvars64.bat in known Visual Studio / Build Tools locations, with a
rem  filesystem search fallback.
rem ============================================================================
set "ROOTDIR=%~dp0"
set ROOTDIR=%ROOTDIR:~0,-1%
set "VCVARS="
for %%V in ("C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2026\Community\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2026\Professional\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2026\Enterprise\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2026\BuildTools\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files (x86)\Microsoft Visual Studio\2026\BuildTools\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" "C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars64.bat") do if not defined VCVARS if exist %%V set "VCVARS=%%V"
set VCVARS=%VCVARS:"=%
if not defined VCVARS for /f "usebackq delims=" %%F in (`powershell -NoProfile -Command "Get-ChildItem 'C:\Program Files','C:\Program Files (x86)' -Recurse -Depth 7 -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName"`) do set "VCVARS=%%F"
set VCVARS=%VCVARS:"=%
if not defined VCVARS echo ERROR: vcvars64.bat not found. Install Visual Studio or Build Tools with "Desktop development with C++".
if not defined VCVARS exit /b 1
echo Using: %VCVARS%
call "%VCVARS%" >nul 2>&1
if not defined INCLUDE echo ERROR: vcvars64.bat did not set up the compiler environment.
if not defined INCLUDE exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /DUNICODE /D_UNICODE /Fo"%ROOTDIR%\\" /Fe"%ROOTDIR%\ark_env.exe" "%ROOTDIR%\ark_env.cpp" /link psapi.lib version.lib advapi32.lib user32.lib kernel32.lib
if errorlevel 1 exit /b 1
echo.
echo BUILD OK: %ROOTDIR%\ark_env.exe
exit /b 0
