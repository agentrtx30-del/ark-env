@echo off
rem Run from ark-env root (x64 MSVC prompt).
if not exist inject\build mkdir inject\build
cl /nologo /std:c++17 /EHsc /W2 /DUNICODE /D_UNICODE ^
inject\inject_main.cpp /Fe:inject\build\ark-esp-inject.exe ^
/link /SUBSYSTEM:CONSOLE psapi.lib advapi32.lib
cl /nologo /std:c++17 /EHsc /W2 /LD /DUNICODE /D_UNICODE ^
inject\arkesp_dll.cpp /Fe:inject\build\arkesp.dll ^
/link d3d11.lib d2d1.lib dwrite.lib psapi.lib
copy /Y inject\build\arkesp.dll inject\arkesp.dll >nul
echo done: inject\build\ark-esp-inject.exe + inject\build\arkesp.dll