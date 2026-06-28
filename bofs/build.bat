@echo off
REM Build BOF (.c -> .o) for the LRD BOF loader.
REM
REM Supports two compilers (auto-detected):
REM   1. MinGW-w64: x86_64-w64-mingw32-gcc -c file.c -o file.o
REM   2. MSVC:      cl /c /GS- file.c /Fo file.o
REM      (Run from "x64 Native Tools Command Prompt for VS")
REM
REM Usage:
REM   cd bofs
REM   build.bat                   (build all .c files)
REM   build.bat bof_whoami.c      (build one file)

setlocal

REM --- Detect compiler ---
where x86_64-w64-mingw32-gcc >nul 2>&1
if %errorlevel% == 0 (
    set CC=x86_64-w64-mingw32-gcc
    set MODE=mingw
    goto :found
)

where cl.exe >nul 2>&1
if %errorlevel% == 0 (
    set CC=cl.exe
    set MODE=msvc
    goto :found
)

echo [!] No C compiler found.
echo     Install MinGW-w64 or run from VS x64 Native Tools Command Prompt.
exit /b 1

:found
echo [*] Using %MODE% compiler: %CC%

if "%1" NEQ "" (
    call :build_one %1
    exit /b %errorlevel%
)

echo [*] Building all .c files (including subdirectories) ...
for /r %%f in (*.c) do call :build_one "%%f"

echo [*] Done.
exit /b 0

:build_one
set SRC=%1
set OBJ=%~n1.o
if "%MODE%" == "mingw" (
    echo [*] %SRC% -^> %OBJ% [mingw]
    %CC% -c %SRC% -o %OBJ%
) else (
    echo [*] %SRC% -^> %OBJ% [msvc]
    %CC% /c /GS- %SRC% /Fo%OBJ%
)
if errorlevel 1 (
    echo [!] FAILED: %SRC%
    exit /b 1
)
echo [+] OK: %OBJ%
exit /b 0
