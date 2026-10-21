@echo off
rem Build the Link: The Faces of Evil static recompilation.
rem Requirements: Python 3, CMake, Visual Studio 2022 (C compiler), and the
rem game's disc image (.bin/.cue) in the rom\ folder.
setlocal
cd /d "%~dp0"

echo [1/3] Recompiling 68000 code to C...
python tools\gen.py || goto :error

echo [2/3] Configuring...
cmake -S . -B build\cmake -G "Visual Studio 17 2022" -A x64 > nul || goto :error

echo [3/3] Compiling (this takes a minute)...
cmake --build build\cmake --config Release --target linkfoe linkfoe_hd --parallel || goto :error

copy /y build\cmake\Release\linkfoe.exe linkfoe.exe > nul
copy /y build\cmake\Release\linkfoe_hd.exe linkfoe_hd.exe > nul
echo.
echo Done. Run linkfoe.exe (original look) or linkfoe_hd.exe (HD upscaling) to play.
exit /b 0

:error
echo Build failed.
exit /b 1
