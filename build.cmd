@echo off
rem Builds the 32-bit d3d9.dll proxy with Visual Studio 2026 Insiders (never VS 2022).
rem Usage: build.cmd [deploy]   - "deploy" copies the DLL into the game folder.
setlocal
set VS=C:\Program Files\Microsoft Visual Studio\18\Insiders
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
set GAME=H:\SteamLibrary\steamapps\common\Geometry Wars 3 - Dimensions
if /i "%1"=="deploy" copy /y "%OUT%\d3d9.dll" "%GAME%\" >nul && copy /y "%OUT%\openxr_loader.dll" "%GAME%\" >nul && echo deployed
endlocal
