@echo off
rem Builds s2fsr.dll (AMD FSR 3.1 and the camera motion vectors behind s2fsr.h) and its offline tests with 32-bit MSVC.
rem Usage: build.cmd [out dir] [FSR SDK folder]; by default build\ here and ..\fsr-sdk, fetched or checked first.
setlocal
set HERE=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%HERE%build
set SDK=%~2
if "%SDK%"=="" set SDK=%HERE%..\fsr-sdk
set S=%SDK%\sdk
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo s2fsr: no Visual Studio & exit /b 1)
for /f "usebackq tokens=*" %%i in (`call "%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo s2fsr: no MSVC x86 tools & exit /b 1)
python -I "%HERE%get_fsr_sdk.py" "%SDK%" || exit /b 1
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" amd64_x86 >nul || exit /b 1
rem The compiler's version goes in the build log: releases are built with whatever MSVC the build machine has.
cl 2>&1 | findstr /c:"Version"
python -I "%HERE%shaders.py" "%SDK%" "%OUT%\fsr_shaders" || exit /b 1
python -I "%HERE%patch_backend.py" "%SDK%" "%OUT%\fsr_backend" || exit /b 1
fxc /nologo /T cs_5_0 /E main /O3 /Vn motion_cs /Fh "%OUT%\motion_cs.h" "%HERE%motion.hlsl" >nul || exit /b 1
fxc /nologo /T cs_5_0 /E main /O3 /Vn mask_cs /Fh "%OUT%\mask_cs.h" "%HERE%mask.hlsl" >nul || exit /b 1
if not exist "%OUT%\obj\tests" mkdir "%OUT%\obj\tests"
del /q "%OUT%\obj\*.obj" 2>nul
set INC=/I"%S%\include" /I"%S%\src\backends\shared" /I"%S%\src\components" /I"%S%\src\shared" /I"%OUT%\fsr_shaders"
rem FFX_GCC only turns FidelityFX's dllexport off. /EHs, not /EHsc: the backend throws through FidelityFX's extern "C" functions.
cl /nologo /c /O2 /MT /EHs /std:c++17 /W3 /DFFX_GCC /D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING %INC% ^
  "%S%\src\components\fsr3upscaler\ffx_fsr3upscaler.cpp" ^
  "%OUT%\fsr_backend\ffx_dx11.cpp" "%S%\src\backends\shared\blob_accessors\ffx_fsr3upscaler_shaderblobs.cpp" ^
  "%S%\src\shared\ffx_object_management.cpp" "%S%\src\shared\ffx_assert.cpp" /Fo"%OUT%\obj/" || exit /b 1
cl /nologo /c /O2 /MT /W3 "%S%\src\backends\dx11\DXBCChecksum.c" "%S%\src\backends\dx11\md5.c" /Fo"%OUT%\obj/" || exit /b 1
cl /nologo /c /O2 /MT /EHs /std:c++17 /W4 /WX /DFFX_GCC /external:W0 /external:I"%S%\include" ^
  /external:I"%S%\src\backends\shared" /I"%OUT%" "%HERE%s2fsr.cpp" "%HERE%shaderblobs.cpp" /Fo"%OUT%\obj/" || exit /b 1
rem Version info, when the caller wrote it as s2fsr.res beforehand (s2fix's build does).
set RES=
if exist "%OUT%\s2fsr.res" set RES="%OUT%\s2fsr.res"
link /nologo /DLL /OUT:"%OUT%\s2fsr.dll" /IMPLIB:"%OUT%\obj\s2fsr.lib" "%OUT%\obj\*.obj" %RES% d3d11.lib dxguid.lib || exit /b 1
for %%t in (test_fsrdll test_fsrjit) do (
  cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /WX /I"%OUT%" "%HERE%%%t.cpp" /Fo"%OUT%\obj\tests/" /Fe"%OUT%\%%t.exe" ^
    d3d11.lib dxgi.lib || exit /b 1
)
