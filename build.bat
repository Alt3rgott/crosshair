@echo off
rem Run from "x64 Native Tools Command Prompt for VS"
rc /nologo crosshair.rc
cl /nologo /O2 /EHsc /std:c++17 /utf-8 crosshair.cpp crosshair.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO
