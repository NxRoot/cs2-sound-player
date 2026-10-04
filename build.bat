@echo off
setlocal

set "CLANG=C:\Program Files (x86)\windows-cpp\clang++.exe"
set "OUT=output\portable\CS2SP.exe"
set "RES=sounds.res"

mkdir output >nul 2>&1
mkdir output\\portable >nul 2>&1

@REM The icon has to go through a resource compiler, and the clang install here
@REM ships no llvm-rc, so borrow rc.exe from the newest installed Windows SDK.
@REM sounds.rc needs no headers, so the SDK include paths never come into it.
set "RC="
set "KITS=C:\Program Files (x86)\Windows Kits\10\bin"
for /f "delims=" %%v in ('dir /b /o-n "%KITS%\10.*" 2^>nul') do (
    if not defined RC if exist "%KITS%\%%v\x64\rc.exe" set "RC=%KITS%\%%v\x64\rc.exe"
)

echo.
echo Building %OUT% ...
echo.

if not defined RC (
    echo Warning: no Windows SDK rc.exe found, building without the icon.
    set "RES="
) else (
    "%RC%" /nologo /fo "%RES%" "sounds.rc"
    if errorlevel 1 (
        echo.
        echo Resource compile failed.
        pause
        exit /b 1
    )
)

"%CLANG%" -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -municode -fms-extensions ^
    "sounds.cpp" %RES% ^
    -o "%OUT%" ^
    -luser32 -lkernel32 -lwinmm -lole32 -lmfplat -lmfreadwrite -lmfuuid -lpsapi

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build failed.
    pause
    exit /b %ERRORLEVEL%
)

if exist "%RES%" del "%RES%"

"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /O"C:\Users\Administrator\Desktop\Code\cssound\output" sounds.iss

echo Output File: %OUT%
echo.
