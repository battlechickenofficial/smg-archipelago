@echo off
rem Builds plugins\smg_archipelago.dll with MSVC (Visual Studio 2022 or Build Tools with "Desktop development with C++").
setlocal
set HERE=%~dp0
set VCVARS=
for %%V in (
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist %%V set VCVARS=%%V
if not defined VCVARS (
  echo Could not find vcvars64.bat. Install Visual Studio Build Tools with the C++ workload.
  exit /b 1
)
call %VCVARS% >nul || exit /b 1
if not exist "%HERE%plugins" mkdir "%HERE%plugins"
if not exist "%HERE%build" mkdir "%HERE%build"
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /I"%HERE%include" /Fo"%HERE%build\\" ^
   "%HERE%src\smg_archipelago.cpp" /LD /Fe"%HERE%plugins\smg_archipelago.dll" ^
   /link ws2_32.lib kernel32.lib /NOLOGO || exit /b 1
echo Built %HERE%plugins\smg_archipelago.dll
