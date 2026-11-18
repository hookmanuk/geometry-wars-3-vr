@echo off
rem Builds the 32-bit d3d9.dll proxy.
rem Usage: build.cmd [deploy]   - "deploy" also copies the DLLs into the game folder.
rem Set VS to your Visual Studio folder and GW3_DIR to the game folder if they differ from below.
setlocal
if "%VS%"=="" set VS=C:\Program Files\Microsoft Visual Studio\18\Insiders
if "%GW3_DIR%"=="" set GW3_DIR=C:\Program Files (x86)\Steam\steamapps\common\Geometry Wars 3 - Dimensions
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set ROOT=%~dp0
set OUT=%ROOT%out
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set XR=
if exist "%ROOT%deps\openxr-build\src\loader\Release\openxr_loader.lib" set XR="%ROOT%deps\openxr-build\src\loader\Release\openxr_loader.lib"
cl /nologo /std:c++20 /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%deps\openxr-sdk\include" ^
   /Fo"%OUT%\obj\\" /Fe"%OUT%\d3d9.dll" /LD "%ROOT%src\*.cpp" ^
   /link /DEF:"%ROOT%src\d3d9.def" /IMPLIB:"%OUT%\obj\d3d9.lib" %XR% dxguid.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib advapi32.lib ole32.lib pathcch.lib || exit /b 1
copy /y "%ROOT%deps\openxr-build\src\loader\Release\openxr_loader.dll" "%OUT%\" >nul
copy /y "%ROOT%deps\openxr-sdk\LICENSE" "%OUT%\openxr_loader-LICENSE.txt" >nul
if /i "%1"=="deploy" copy /y "%OUT%\d3d9.dll" "%GW3_DIR%\" >nul && copy /y "%OUT%\openxr_loader.dll" "%GW3_DIR%\" >nul && echo deployed to %GW3_DIR%
endlocal
