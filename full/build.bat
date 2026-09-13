@echo off
rem Run from the full\ directory.
if not exist build mkdir build
if not exist build\obj mkdir build\obj

cl /nologo /std:c++17 /EHsc /W3 /DUNICODE /D_UNICODE ^
   /Iinclude /I..\overlay ^
   /Fobuild\obj\ ^
   src\main.cpp ^
   src\log.cpp ^
   src\memory.cpp ^
   src\offsets.cpp ^
   src\process.cpp ^
   src\camera.cpp ^
   src\actors.cpp ^
   src\targets.cpp ^
   src\projection.cpp ^
   src\selfcheck.cpp ^
   src\nametable.cpp ^
   src\classifier.cpp ^
   ..\overlay\arkoverlay.cpp ^
   /Fe:build\ark-full.exe ^
   /link /SUBSYSTEM:CONSOLE