@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0src"
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /LD KuroMP4.cpp /Fe:..\KuroMP4.dll /link /DLL
