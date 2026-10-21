@echo off
rem Build the portable zip and the installer into dist\ (see tools\package.ps1).
rem   package.bat              build, then package
rem   package.bat -SkipBuild   package the current build
rem   package.bat -Publish     also publish a GitHub release (update source)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package.ps1" %*
