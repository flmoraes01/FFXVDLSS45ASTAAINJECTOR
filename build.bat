@echo off
rem Builds winmm.dll (FFXV DLSS4 mod). Requires VS 2022 Build Tools and the DLSS SDK (DLSS_SDK env var).
setlocal
if "%DLSS_SDK%"=="" set DLSS_SDK=%~dp0third_party\DLSS
set MINHOOK=%~dp0third_party\minhook
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%~dp0build" mkdir "%~dp0build"
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DLSS_SDK%\include" /I "%MINHOOK%\include" /Fo"%~dp0build\\" "%~dp0src\main.cpp" ^
   "%MINHOOK%\src\hook.c" "%MINHOOK%\src\buffer.c" "%MINHOOK%\src\trampoline.c" "%MINHOOK%\src\hde\hde64.c" ^
   /link /OUT:"%~dp0build\winmm.dll" /IMPLIB:"%~dp0build\winmm.lib" /LIBPATH:"%DLSS_SDK%\lib\Windows_x86_64\x64" nvsdk_ngx_s.lib user32.lib advapi32.lib
exit /b %ERRORLEVEL%
