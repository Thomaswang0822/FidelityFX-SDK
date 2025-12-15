:: This file is part of the FidelityFX SDK.
::
:: Copyright (C) 2024 Advanced Micro Devices, Inc.
::
:: Permission is hereby granted, free of charge, to any person obtaining a copy
:: of this software and associated documentation files(the "Software"), to deal
:: in the Software without restriction, including without limitation the rights
:: to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
:: copies of the Software, and to permit persons to whom the Software is
:: furnished to do so, subject to the following conditions :
::
:: The above copyright notice and this permission notice shall be included in
:: all copies or substantial portions of the Software.
::
:: THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
:: IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
:: FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
:: AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
:: LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
:: OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
:: THE SOFTWARE.

@echo off

echo ===============================================================
echo.
echo  FidelityFX Samples Build System
echo.
echo ===============================================================
echo.

echo Using fixed development configuration:
echo - FFX_DEV_API=1 (Development mode enabled)
echo - RUNTIME_SHADER_RECOMPILE=0 (No hot reload)
echo - FFX_AUTO_COMPILE_SHADERS=1 (Auto compile shaders at build time)
echo - Building ALL components
echo - Using static libraries (recommended for development)
echo.

set samples_build_options=-DRUNTIME_SHADER_RECOMPILE=0 -DFFX_ALL=ON
set sdk_build_options=-DFFX_AUTO_COMPILE_SHADERS=1 -DFFX_ALL=ON

:: determine architecture
if /i "%PROCESSOR_ARCHITECTURE%" == "ARM64" (
    set arch=ARM64
    set samples_build_options=-AARM64 -DCMAKE_GENERATOR_PLATFORM=ARM64 %samples_build_options%
    set sdk_build_options=-AARM64 -DCMAKE_GENERATOR_PLATFORM=ARM64 %sdk_build_options%
) else (
    set arch=X64
    set samples_build_options=-Ax64 -DCMAKE_GENERATOR_PLATFORM=x64 %samples_build_options%
    set sdk_build_options=-Ax64 -DCMAKE_GENERATOR_PLATFORM=x64 %sdk_build_options%
)
echo architecture %arch% detected
echo.

:: Add FFX_DEV_API flag
set samples_build_options=-DFFX_DEV_API=1 %samples_build_options%

set build_type=-DBUILD_TYPE=SAMPLES_DX12
set sdk_build_options=-DFFX_API_BACKEND=DX12_%arch% %sdk_build_options%

:: Only build SDK separately if NOT in development mode; we are in
echo Skipping SDK prebuild - SDK will be included in solution

:: Remove then create new
if exist bin\ (
    rmdir /S /Q bin\
)
:: Keep build/.vs so that we don't need to reopen every file
if exist build\ (
    rm -Recurse build\*
) else (
    mkdir build
)
cd build

echo.
echo Building SDK sample solutions %samples_build_options%
echo.

cmake .. %build_type% %samples_build_options%

:: Come back to root level
cd ..
pause
