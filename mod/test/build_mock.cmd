@echo off
rem Builds the mock Nebula host used by test_plugin.py.
setlocal
set HERE=%~dp0
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not exist "%HERE%build" mkdir "%HERE%build"
cl /nologo /std:c++17 /O2 /EHsc /W3 /I"%HERE%..\include" /Fo"%HERE%build\\" "%HERE%mock_host.cpp" /Fe"%HERE%build\mock_host.exe" || exit /b 1
