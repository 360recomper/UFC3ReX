@echo off
rem Release build of ufc3.exe for the download (tools\release.ps1 packages it).
rem Same as build.bat but the win-amd64-release preset; the DLLs come from the SDK's non-rd set.
call "%~dp0..\env.bat"
cd /d "%~dp0.."
if not defined UFC3_SDK set "UFC3_SDK=%~dp0..\..\sdk-install"
set "SDK=%UFC3_SDK%"
if not exist assets\default.xex (
  echo assets\default.xex missing - extract the ISO with tools\xiso_extract.py
  exit /b 1
)
"%SDK%\bin\rexglue.exe" codegen ufc3_manifest.toml || exit /b 1
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH="%SDK%" -Drexglue_DIR="%SDK%\lib\cmake\rexglue" || exit /b 1
cmake --build --preset win-amd64-release || exit /b 1
set "OUT=out\build\win-amd64-release"
for %%f in (rexruntime.dll rexgpu-xenos.dll) do (
  copy /y "%SDK%\bin\%%f" "%OUT%\" >nul || exit /b 1
)
echo.
echo Built: %CD%\%OUT%\ufc3.exe
