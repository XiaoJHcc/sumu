@echo off
setlocal
rem Kill leftover player processes first: deferred model unload keeps python.exe alive
rem after the window closes, and a running process locks sumu_core.pyd -> LNK1104 at link.
rem Match is limited to this repo's entrypoints so unrelated python processes are spared.
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -match 'play\.py|run_player\.py|sumu\.app|sumu_core' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DPython_EXECUTABLE=d:/Git/sumu/.venv/Scripts/python.exe ^
  -Dpybind11_DIR=d:/Git/sumu/.venv/Lib/site-packages/pybind11/share/cmake/pybind11
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
echo BUILD_OK
