@echo off
rem Usage: scripts\idf_build.bat <example_dir> <target> [extra idf.py args]
rem e.g.   scripts\idf_build.bat examples\s3 esp32s3
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;%PATH%
call C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat
if errorlevel 1 exit /b 1
cd /d %~dp0..\%1
if errorlevel 1 exit /b 1
idf.py set-target %2
if errorlevel 1 exit /b 1
idf.py build %3 %4 %5
