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
"%SDK%\bin\rexglue.exe" codegen ufc3_manifest.toml || exit /b 1
cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH="%SDK%" -Drexglue_DIR="%SDK%\lib\cmake\rexglue" || exit /b 1
cmake --build --preset win-amd64-relwithdebinfo || exit /b 1
set "OUT=out\build\win-amd64-relwithdebinfo"
for %%f in (rexruntimerd.dll rexgpu-xenosrd.dll TracyClientrd.dll) do (
  copy /y "%SDK%\bin\%%f" "%OUT%\" >nul || exit /b 1
)
echo.
echo Built: %CD%\%OUT%\ufc3.exe
