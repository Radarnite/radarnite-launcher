@echo off
rem Build RadarniteUI.exe with zig on Windows: build.cmd [out.exe]  (ZIG = path to zig.exe; flags in build.rsp)
setlocal
cd /d "%~dp0"
if "%ZIG%"=="" set ZIG=zig
set OUT=%~1
if "%OUT%"=="" set OUT=RadarniteUI.exe
"%ZIG%" rc /fo app.res app.rc || exit /b 1
"%ZIG%" c++ @build.rsp -o "%OUT%" || exit /b 1
del app.res
for %%f in ("%OUT%") do echo %%~zf %%~nxf
