@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "D:\SteamLibrary\steamapps\common\Sub Rosa\Suitium"
cl /nologo /std:c++20 /EHsc /I Sandium Tools\poser\offsets.cpp /Fe:Tools\poser\offsets.exe
Tools\poser\offsets.exe
