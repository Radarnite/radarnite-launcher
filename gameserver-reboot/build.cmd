@echo off
rem MSVC build of Project Reboot 3.0 (fnoffline patch) with the portable MSVC 2022 toolchain. Mirrors the vcxproj Release|x64.
setlocal
call "%LOCALAPPDATA%\fp-tools\msvc-dl\msvc\setup_x64.bat"
cd /d "%~dp0"
if not exist obj mkdir obj
if not exist obj\mh mkdir obj\mh
if not exist out mkdir out
cl /nologo /c /MT /O2 /Oi /Gy /GL /W3 /DNDEBUG /Iminhook\include /Foobj\mh\ minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde64.c || exit /b 1
lib /nologo /LTCG /out:out\minhook.x64.lib obj\mh\*.obj || exit /b 1
cd "Project Reboot 3.0"
set IMGUI=..\vendor\ImGui\imgui.cpp ..\vendor\ImGui\imgui_demo.cpp ..\vendor\ImGui\imgui_draw.cpp ..\vendor\ImGui\imgui_impl_dx9.cpp ..\vendor\ImGui\imgui_impl_win32.cpp ..\vendor\ImGui\imgui_stdlib.cpp ..\vendor\ImGui\imgui_tables.cpp ..\vendor\ImGui\imgui_widgets.cpp
cl /nologo /c /MP%JOBS% /MT /O2 /Oi /Gy /GL /sdl /wd4703 /permissive- /W3 /EHsc /std:c++latest /utf-8 /Zi /Fd..\obj\vc.pdb /DNDEBUG /DPROJECTREBOOT30_EXPORTS /D_WINDOWS /D_USRDLL /DUNICODE /D_UNICODE /DSPDLOG_USE_STD_FORMAT /DFNOFFLINE_DEBUG_LOG /I..\mhinc /I..\vendor /Fo..\obj\ *.cpp %IMGUI% > ..\out\compile.log 2>&1
if errorlevel 1 ( findstr /i /c:" error " ..\out\compile.log & exit /b 1 )
cd ..
link /nologo /dll /machine:x64 /subsystem:windows /debug /LTCG /opt:ref /opt:icf /out:out\radarnite-server.dll /pdb:out\radarnite-server.pdb obj\*.obj out\minhook.x64.lib Oleaut32.lib Onecore.lib d3d9.lib dwmapi.lib gdi32.lib imm32.lib kernel32.lib user32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib uuid.lib > out\link.log 2>&1
if errorlevel 1 ( type out\link.log & exit /b 1 )
echo BUILD OK
