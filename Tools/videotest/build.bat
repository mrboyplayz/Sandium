@echo on
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
where cl
cl /nologo /EHsc /O2 main.cpp /Fe:videotest.exe
