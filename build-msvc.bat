@echo off
rem Builds RotatingQuicksaves.asi with Microsoft Visual C++.
rem Run from an "x64 Native Tools Command Prompt for VS" in this folder.
cl /nologo /O2 /W3 /LD /MT src\RotatingQuicksaves.c /link /OUT:RotatingQuicksaves.asi kernel32.lib
del /q RotatingQuicksaves.obj RotatingQuicksaves.lib RotatingQuicksaves.exp 2>nul
