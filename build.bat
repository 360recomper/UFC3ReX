@echo off
rem Codegen + configure + build UFC Undisputed 3 against a UFC-only ReXGlue SDK:
rem upstream nightly-20261002 plus sdk-patch\ (see BUILDING.md). UFC3_SDK = the SDK install folder.
call "%~dp0env.bat"
cd /d "%~dp0"
if not defined UFC3_SDK set "UFC3_SDK=%~dp0..\sdk-install"
set "SDK=%UFC3_SDK%"
if not exist assets\default.xex (
  echo assets\default.xex missing - extract the ISO with tools\xiso_extract.py
  exit /b 1
)
rem build.bat native / nonative: compile the native renderer in or out (kept in the CMake cache).
rem Release builds use "native" (run python -I tools\nr_fetch_third_party.py once first).
set "NATIVE_ARG="
if /i "%~1"=="native" set "NATIVE_ARG=-DUFC3_NATIVE_RENDERER=ON -DUFC3_NATIVE_HOOKS=ON"
if /i "%~1"=="nonative" set "NATIVE_ARG=-DUFC3_NATIVE_RENDERER=OFF"
"%SDK%\bin\rexglue.exe" codegen ufc3_manifest.toml || exit /b 1
cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH="%SDK%" -Drexglue_DIR="%SDK%\lib\cmake\rexglue" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" %NATIVE_ARG% || exit /b 1
cmake --build --preset win-amd64-relwithdebinfo || exit /b 1
set "OUT=out\build\win-amd64-relwithdebinfo"
for %%f in (rexruntimerd.dll rexgpu-xenosrd.dll TracyClientrd.dll) do (
  copy /y "%SDK%\bin\%%f" "%OUT%\" >nul || exit /b 1
)
echo.
echo Built: %CD%\%OUT%\ufc3.exe
