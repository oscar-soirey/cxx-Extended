@echo off
REM Build the CXXE language server with MinGW-w64 gcc.
REM The server reuses the compiler modules of Compiler\Src (everything except
REM the command line entry point Cli\Main.c).
REM Usage: Tools\Ls\Build.bat [output.exe]
REM   Default output: Tools\Ls\cxxe-language-server.exe, also copied into the
REM   VS Code extension (Tools\Extensions\Vscode\Source\server\win32-x64\).
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
set "COMPILER=%ROOT%..\..\Compiler\Src"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%ROOT%cxxe-language-server.exe"
set "EXT=%ROOT%..\Extensions\Vscode\Source\server"

where gcc >nul 2>nul
if errorlevel 1 (
    echo error: gcc not found in PATH, install MinGW-w64 first.
    exit /b 1
)

set "SRCS="
for %%f in ("%ROOT%Src\*.c") do set SRCS=!SRCS! "%%f"
for /r "%COMPILER%" %%f in (*.c) do (
    if /I not "%%~nxf"=="Main.c" set SRCS=!SRCS! "%%f"
)

echo Building %OUT%
gcc -std=c11 -O2 -Wall -Wextra -static -I"%COMPILER%" -I"%ROOT%Src" !SRCS! -o "%OUT%"
if errorlevel 1 exit /b 1

"%OUT%" --version
if errorlevel 1 exit /b 1

if exist "%EXT%" (
    if not exist "%EXT%\win32-x64" mkdir "%EXT%\win32-x64"
    copy /Y "%OUT%" "%EXT%\win32-x64\cxxe-language-server.exe" >nul
    if errorlevel 1 exit /b 1
    echo Copied to %EXT%\win32-x64\cxxe-language-server.exe
)
exit /b 0
