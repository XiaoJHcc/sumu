@echo off
setlocal
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
