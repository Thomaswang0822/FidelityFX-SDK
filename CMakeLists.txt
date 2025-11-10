# This file is part of the FidelityFX SDK.
#
# Copyright (C) 2024 Advanced Micro Devices, Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files(the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions :
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

cmake_minimum_required(VERSION 3.23)

if (NOT DEFINED BUILD_TYPE)
    message(FATAL_ERROR "No BUILD_TYPE specified!)")
endif()

if( BUILD_TYPE STREQUAL CAULDRON)
	# Standalone cauldron builds VK and DX12
	set(CAULDRON_VK 1)
	set(CAULDRON_DX12 1)
	set(SOLUTION_NAME "Cauldron")
elseif(BUILD_TYPE STREQUAL SAMPLES_DX12)
	set(CAULDRON_DX12 1)
	set(SOLUTION_NAME "FidelityFX SDK DX12 Samples")
elseif(BUILD_TYPE STREQUAL SAMPLES_VK)
	set(CAULDRON_VK 1)
	set(SOLUTION_NAME "FidelityFX SDK VK Samples")
else()
	message(FATAL_ERROR "Invalid BUILD_TYPE specified! BUILD_TYPE must be one of CAULDRON, SAMPLES_VK, SAMPLES_DX12")
endif()

project ("${SOLUTION_NAME}" VERSION 1.0.0 LANGUAGES C CXX)
message(STATUS "Building FidelityFX SDK Samples")

# Pull in common definitions and functions
list( APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} )
include(common)
include(sample)

# Configuration setup
if (CMAKE_GENERATOR_PLATFORM STREQUAL x64 OR
	CMAKE_GENERATOR_PLATFORM STREQUAL ARM64 OR
	CMAKE_GENERATOR_PLATFORM STREQUAL ARM64EC)
    message(STATUS "Creating sample configs ...")
	set(CMAKE_CONFIGURATION_TYPES "")
	if (CAULDRON_DX12)
		createConfig(DX12 DebugDX12 DEBUG)
		createConfig(DX12 RelWithDebInfoDX12 RELWITHDEBINFO)
		createConfig(DX12 ReleaseDX12 RELEASE)
		list(APPEND CMAKE_CONFIGURATION_TYPES "DebugDX12;RelWithDebInfoDX12;ReleaseDX12;")
	endif()

	if (CAULDRON_VK)
		createConfig(VK DebugVK DEBUG)
		createConfig(VK RelWithDebInfoVK RELWITHDEBINFO)
		createConfig(VK ReleaseVK RELEASE)
		list(APPEND CMAKE_CONFIGURATION_TYPES "DebugVK;RelWithDebInfoVK;ReleaseVK;")
	endif()

    message(STATUS "Generated with config types: ${CMAKE_CONFIGURATION_TYPES}")

    # Set compile definitions as well
    set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
        $<$<CONFIG:DebugDX12>:_DX12 _WIN>
        $<$<CONFIG:DebugVK>:_VK _WIN>
        $<$<CONFIG:RelWithDebInfoDX12>:_DX12 _WIN _RELEASE _RELEASEWDEBUG>
        $<$<CONFIG:RelWithDebInfoVK>:_VK _WIN _RELEASE _RELEASEWDEBUG>
        $<$<CONFIG:ReleaseDX12>:_DX12 _WIN _RELEASE>
        $<$<CONFIG:ReleaseVK>:_VK _WIN _RELEASE>
        NOMINMAX
    )

else()
    message(FATAL_ERROR "Requested architecture ${CMAKE_GENERATOR_PLATFORM} not yet supported!)")
endif()

# If FFX_DEV_API, let targets under samples/ look for .lib under bin/ffx_sdk instead of sdk/bin/ffx_sdk
if(FFX_DEV_API)
    set(FFX_SDK_LIB_PATH "${BIN_OUTPUT}/ffx_sdk")
else()
    set(FFX_SDK_LIB_PATH "${SDK_ROOT}/bin/ffx_sdk")
endif()
set(FFX_SDK_LIB_PATH "${FFX_SDK_LIB_PATH}" CACHE PATH "FidelityFX SDK library path")

# Similar thing for ffx-api/
if(FFX_DEV_API)
    set(FFX_API_LIB_PATH "${BIN_OUTPUT}/ffx-api")
else()
    set(FFX_API_LIB_PATH "${CMAKE_SOURCE_DIR}/ffx-api/bin")
endif()
set(FFX_API_LIB_PATH "${FFX_API_LIB_PATH}" CACHE PATH "FidelityFX API library path")

# Output exe to bin directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BIN_OUTPUT})
foreach( OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES} )
    string( TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG )
    set( CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${BIN_OUTPUT} )
endforeach( OUTPUTCONFIG CMAKE_CONFIGURATION_TYPES )

if (RUNTIME_SHADER_RECOMPILE EQUAL 1)
    set(FFX_BUILD_AS_DLL 1)
    set(SUPPORT_RUNTIME_SHADER_RECOMPILE 1)
endif()

# Since we have conditionally set ${FFX_SDK_LIB_PATH}, adding ffx_backend_native can be globally.
add_library(ffx_backend_native STATIC IMPORTED)
set_target_properties(ffx_backend_native PROPERTIES
                        IMPORTED_CONFIGURATIONS "DebugDX12;DebugVK;RelWithDebInfoDX12;RelWithDebInfoVK;ReleaseDX12;ReleaseVK"
                        IMPORTED_LOCATION_DEBUGDX12 "${FFX_SDK_LIB_PATH}/ffx_backend_dx12_${CMAKE_GENERATOR_PLATFORM}d.lib"
                        IMPORTED_LOCATION_RELEASEDX12 "${FFX_SDK_LIB_PATH}/ffx_backend_dx12_${CMAKE_GENERATOR_PLATFORM}.lib"
                        IMPORTED_LOCATION_RELWITHDEBINFODX12 "${FFX_SDK_LIB_PATH}/ffx_backend_dx12_${CMAKE_GENERATOR_PLATFORM}drel.lib"
                        IMPORTED_LOCATION_DEBUGVK "${FFX_SDK_LIB_PATH}/ffx_backend_vk_${CMAKE_GENERATOR_PLATFORM}d.lib"
                        IMPORTED_LOCATION_RELEASEVK "${FFX_SDK_LIB_PATH}/ffx_backend_vk_${CMAKE_GENERATOR_PLATFORM}.lib"
                        IMPORTED_LOCATION_RELWITHDEBINFOVK "${FFX_SDK_LIB_PATH}/ffx_backend_vk_${CMAKE_GENERATOR_PLATFORM}drel.lib")

# Import backend libs - only if NOT in development mode
if(NOT FFX_DEV_API)
    message(STATUS "SDK source are delivered as prebuilt libs in solution")

    if (FFX_BUILD_AS_DLL)
        file(GLOB SDK_DLLS "${FFX_SDK_LIB_PATH}/*.dll")
        message(STATUS "Copying ${SDK_DLLS} to bin.")
        copyTargetCommand("${SDK_DLLS}" ${CMAKE_HOME_DIRECTORY}/bin copied_sdk_dlls)
        add_dependencies(ffx_backend_native copied_sdk_dlls)
    endif()
else()
    # In development mode, include SDK source in solution
    message(STATUS "Including SDK source in solution (development mode)")

    # Set FFX_API_BACKEND based on architecture and API
    if(CAULDRON_DX12)
        if(CMAKE_GENERATOR_PLATFORM STREQUAL x64)
            set(FFX_API_BACKEND "DX12_X64")
        elseif(CMAKE_GENERATOR_PLATFORM STREQUAL ARM64)
            set(FFX_API_BACKEND "DX12_ARM64")
        endif()
    elseif(CAULDRON_VK)
        if(CMAKE_GENERATOR_PLATFORM STREQUAL x64)
            set(FFX_API_BACKEND "VK_X64")
        elseif(CMAKE_GENERATOR_PLATFORM STREQUAL ARM64)
            set(FFX_API_BACKEND "VK_ARM64")
        endif()
    endif()
    message(STATUS "FFX_API_BACKEND set to: ${FFX_API_BACKEND}")

    # Set the same options that ffx-api would normally set
    set(FFX_FSR2 ON)
    set(FFX_FSR3 ON)
    set(FFX_FSR3UPSCALER ON)
    set(FFX_OF ON)
    set(FFX_FI ON)
    set(FFX_AUTO_COMPILE_SHADERS ON)
    set(FFX_BUILD_AS_DLL OFF)

    add_subdirectory(sdk)
endif()

# Add ffx-api project when in development mode
if(FFX_DEV_API)
    message(STATUS "Adding ffx-api project to solution (development mode)")
    add_subdirectory(ffx-api)
endif()

# Pull in cauldron
add_subdirectory(${CAULDRON_ROOT})
set_target_properties(Framework PROPERTIES FOLDER Framework)
if (CAULDRON_DX12)
	set_target_properties(memoryallocator_dx12 PROPERTIES FOLDER Framework)
endif()
if (CAULDRON_VK)
	set_target_properties(memoryallocator_vk PROPERTIES FOLDER Framework)
endif()
set_target_properties(backend_shader_reloader_impl PROPERTIES FOLDER Framework)
set_target_properties(backend_shader_reloader_stub PROPERTIES FOLDER Framework)

# Build rendermodules used by samples as a separate lib
add_subdirectory(${RENDERMODULE_ROOT})
set_target_properties(RenderModules PROPERTIES FOLDER Framework)

# If this is a cauldron build, we are just building the framework for standalone work
if( BUILD_TYPE STREQUAL CAULDRON)
    # Set as startup project
    set_property(DIRECTORY ${CMAKE_PROJECT_DIR} PROPERTY VS_STARTUP_PROJECT Cauldron)
    return()
endif()

# Setup SDK compile definitions
if(CAULDRON_DX12)
	set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS FFX_API_DX12)
endif()
if(CAULDRON_VK)
	set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS FFX_API_VK)
endif()

# Build all other effect samples
add_subdirectory(samples)

# Set a startup project
message(STATUS "Default project set to ${START_PROJECT}")
set_property(DIRECTORY ${CMAKE_PROJECT_DIR} PROPERTY VS_STARTUP_PROJECT ${START_PROJECT})
