@echo off
rem SPDX-License-Identifier: MIT
rem Сборка одной битности: build.cmd x86^|x64 [debug]
rem vswhere -> vcvarsall -> cl/lib/link, без CMake. Артефакты в out\<arch>\.
rem Шаги: core.lib, patch.lib, DLL темы и probe, загрузчик, тестовые DLL и tests.exe,
rem проверка импортов (dumpbin /dependents), прогон tests.exe.
setlocal EnableExtensions EnableDelayedExpansion

set "ARCH=%~1"
set "MODE=%~2"
if /i "%ARCH%"=="x86" goto arch_ok
if /i "%ARCH%"=="x64" goto arch_ok
goto usage
:arch_ok
if "%MODE%"=="" set "MODE=release"
if /i "%MODE%"=="release" goto mode_ok
if /i "%MODE%"=="debug" goto mode_ok
goto usage
:mode_ok
if not "%~3"=="" goto usage

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "OUT=%ROOT%\out\%ARCH%"
set "OBJ=%OUT%\obj"

rem --- окружение MSVC -------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vswhere
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL goto no_vs
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto no_vs
rem vcvarsall сам ищет vswhere.exe в PATH
for %%i in ("%VSWHERE%") do set "PATH=%%~dpi;%PATH%"
call "%VCVARS%" %ARCH% >nul
if errorlevel 1 goto fail_env
where cl >nul 2>nul || goto fail_env

echo [build] %ARCH% %MODE%: %VSINSTALL%

if not exist "%OUT%" mkdir "%OUT%"
for %%d in (core patch theme probe loader testlib testcons testlate tests) do (
  if not exist "%OBJ%\%%d" mkdir "%OBJ%\%%d"
)

rem --- флаги --------------------------------------------------------------
rem /MT: CRT статически, у DLL нет зависимостей от vcruntime/ucrtbase.
set "CFLAGS=/nologo /c /std:c++17 /permissive- /utf-8 /W4 /WX /Zc:__cplusplus /Zc:inline /EHs-c- /GR- /GS /guard:cf /D_HAS_EXCEPTIONS=0 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Zi /FS /I"%ROOT%""
set "LFLAGS=/nologo /DEBUG /DYNAMICBASE /NXCOMPAT /GUARD:CF /INCREMENTAL:NO"
if /i "%MODE%"=="debug" (
  set "CFLAGS=!CFLAGS! /Od /MTd /D_DEBUG /RTC1"
) else (
  set "CFLAGS=!CFLAGS! /O1 /MT /DNDEBUG /Gy /Gw"
  set "LFLAGS=!LFLAGS! /OPT:REF /OPT:ICF"
)
set "SYSLIBS=kernel32.lib user32.lib gdi32.lib"

rem --- библиотеки ядра ----------------------------------------------------
call :compile core "%ROOT%\core\*.cpp" || goto fail
call :mklib core || goto fail
call :compile patch "%ROOT%\patch\*.cpp" || goto fail
call :mklib patch || goto fail

rem --- DLL темы и probe, загрузчик ----------------------------------------
call :compile theme "%ROOT%\theme\*.cpp" || goto fail
call :linkdll theme cfgdark_theme_%ARCH% "%ROOT%\theme\theme.def" "%OBJ%\core.lib %OBJ%\patch.lib %SYSLIBS%" || goto fail
call :compile probe "%ROOT%\probe\*.cpp" || goto fail
call :linkdll probe cfgdark_probe_%ARCH% "%ROOT%\probe\probe.def" "%OBJ%\core.lib %OBJ%\patch.lib %SYSLIBS%" || goto fail
call :compile loader "%ROOT%\loader\*.cpp" || goto fail
call :linkexe loader cfgdark_%ARCH% "kernel32.lib user32.lib" || goto fail

rem --- тестовые модули: поставщик, потребитель с delay-load, поздний модуль -
call :compile testlib "%ROOT%\tests\lib\testlib.cpp" || goto fail
call :linkdll testlib testlib "%ROOT%\tests\lib\testlib.def" "kernel32.lib" || goto fail
call :compile testcons "%ROOT%\tests\lib\testcons.cpp" || goto fail
call :linkdll testcons testcons "" "%OBJ%\testlib.lib delayimp.lib kernel32.lib /DELAYLOAD:testlib.dll" || goto fail
call :compile testlate "%ROOT%\tests\lib\testlate.cpp" || goto fail
call :linkdll testlate testlate "" "%OBJ%\testlib.lib kernel32.lib" || goto fail
call :compile tests "%ROOT%\tests\*.cpp" || goto fail
call :linkexe tests tests "%OBJ%\core.lib %OBJ%\patch.lib %OBJ%\testlib.lib %OBJ%\testcons.lib kernel32.lib" || goto fail

rem --- проверка импортов --------------------------------------------------
rem DLL темы и probe грузятся в 1cv8.exe: только kernel32/user32/gdi32.
rem Загрузчик не рисует: kernel32/user32. Тестовые модули в 1С не попадают,
rem их лишние зависимости - только наши testlib/testcons (цели патч-движка).
call :check_deps "%OUT%\cfgdark_theme_%ARCH%.dll" "kernel32.dll user32.dll gdi32.dll" || goto fail
call :check_deps "%OUT%\cfgdark_probe_%ARCH%.dll" "kernel32.dll user32.dll gdi32.dll" || goto fail
call :check_deps "%OUT%\cfgdark_%ARCH%.exe" "kernel32.dll user32.dll" || goto fail
call :check_deps "%OUT%\testlib.dll" "kernel32.dll" || goto fail
call :check_deps "%OUT%\testcons.dll" "kernel32.dll testlib.dll" || goto fail
call :check_deps "%OUT%\testlate.dll" "kernel32.dll testlib.dll" || goto fail
call :check_deps "%OUT%\tests.exe" "kernel32.dll testlib.dll testcons.dll" || goto fail

rem --- тесты --------------------------------------------------------------
echo [test] %OUT%\tests.exe
"%OUT%\tests.exe" "%ROOT%"
if errorlevel 1 goto fail_tests

echo [build] OK %ARCH% %MODE%
exit /b 0

rem ===========================================================================
:compile
rem %1 - цель, %2 - маска исходников
cl %CFLAGS% /Fo"%OBJ%\%~1\\" /Fd"%OBJ%\%~1\vc.pdb" %2
if errorlevel 1 exit /b 1
exit /b 0

:objlist
rem %1 - цель -> %OBJ%\%1.rsp со списком .obj
if exist "%OBJ%\%~1.rsp" del "%OBJ%\%~1.rsp"
for %%f in ("%OBJ%\%~1\*.obj") do echo "%%~ff">>"%OBJ%\%~1.rsp"
exit /b 0

:mklib
call :objlist %1
lib /nologo /OUT:"%OBJ%\%~1.lib" @"%OBJ%\%~1.rsp"
if errorlevel 1 exit /b 1
exit /b 0

:linkdll
rem %1 - цель, %2 - имя без расширения, %3 - .def или "", %4 - библиотеки
call :objlist %1
set "DEFARG="
if not "%~3"=="" set "DEFARG=/DEF:"%~3""
link %LFLAGS% /DLL /OUT:"%OUT%\%~2.dll" /PDB:"%OUT%\%~2.pdb" /IMPLIB:"%OBJ%\%~2.lib" !DEFARG! @"%OBJ%\%~1.rsp" %~4
if errorlevel 1 exit /b 1
exit /b 0

:linkexe
rem %1 - цель, %2 - имя без расширения, %3 - библиотеки
call :objlist %1
link %LFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUT%\%~2.exe" /PDB:"%OUT%\%~2.pdb" @"%OBJ%\%~1.rsp" %~3
if errorlevel 1 exit /b 1
exit /b 0

:check_deps
rem %1 - файл, %2 - разрешённые зависимости через пробел (обычные и delay-load)
set "CK_ALLOW= %~2 "
set "CK_BAD="
set "CK_LIST="
set /a CK_N=0
for /f "usebackq tokens=1,2" %%a in (`dumpbin /nologo /dependents "%~1"`) do (
  set "CK_T=%%a"
  if "%%b"=="" if /i "!CK_T:~-4!"==".dll" (
    set /a CK_N+=1
    set "CK_LIST=!CK_LIST! %%a"
    rem замена в строке без учёта регистра: имя есть в списке - строка изменится
    set "CK_REST=!CK_ALLOW: %%a =!"
    if "!CK_REST!"=="!CK_ALLOW!" set "CK_BAD=!CK_BAD! %%a"
  )
)
if !CK_N! EQU 0 (
  echo [deps] FAIL %~nx1: no dependencies parsed
  exit /b 1
)
if defined CK_BAD (
  echo [deps] FAIL %~nx1: not allowed:!CK_BAD!
  exit /b 1
)
echo [deps] OK %~nx1:!CK_LIST!
exit /b 0

rem ===========================================================================
:usage
echo usage: build.cmd x86^|x64 [debug]
exit /b 2
:no_vswhere
echo [build] vswhere.exe not found: %VSWHERE%
exit /b 1
:no_vs
echo [build] Visual Studio with VC tools not found
exit /b 1
:fail_env
echo [build] vcvarsall %ARCH% failed
exit /b 1
:fail_tests
echo [build] FAIL: tests %ARCH% %MODE%
exit /b 1
:fail
echo [build] FAIL %ARCH% %MODE%
exit /b 1
