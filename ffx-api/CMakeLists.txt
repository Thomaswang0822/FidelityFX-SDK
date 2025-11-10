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

cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0147)
	# parallel custom command builds (used for shader compilation)
	cmake_policy(SET CMP0147 NEW)
endif()

if (FFX_API_BACKEND STREQUAL VK_X64)
	set(FFX_PLATFORM_NAME vk)
endif()
if (FFX_API_BACKEND STREQUAL DX12_X64 OR
	FFX_API_BACKEND STREQUAL DX12_ARM64 OR
	FFX_API_BACKEND STREQUAL DX12_ARM64EC)
	set(FFX_PLATFORM_NAME dx12)
endif()

project(FFX_API_${FFX_PLATFORM_NAME})

# Include the common.cmake from root to use createConfig function
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/../..)
include(common)

# Set configuration types: use custom ones for dev mode, else standard
if(FFX_DEV_API)
    message(STATUS "Setting ffx-api configurations to match root project")
    
    if(FFX_PLATFORM_NAME STREQUAL "dx12")
        set(CMAKE_CONFIGURATION_TYPES DebugDX12 ReleaseDX12 RelWithDebInfoDX12)
        
        # Use the createConfig function from root project
        createConfig(DX12 DebugDX12 DEBUG)
        createConfig(DX12 ReleaseDX12 RELEASE)
        createConfig(DX12 RelWithDebInfoDX12 RELWITHDEBINFO)

        # Set postfixes for custom configurations
        set(CMAKE_DEBUGDX12_POSTFIX d)
        set(CMAKE_RELWITHDEBINFODX12_POSTFIX drel)
    elseif(FFX_PLATFORM_NAME STREQUAL "vk")
        set(CMAKE_CONFIGURATION_TYPES DebugVK ReleaseVK RelWithDebInfoVK)
        
        createConfig(VK DebugVK DEBUG)
        createConfig(VK ReleaseVK RELEASE)
        createConfig(VK RelWithDebInfoVK RELWITHDEBINFO)

        # Set postfixes for custom configurations
        set(CMAKE_DEBUGVK_POSTFIX d)
        set(CMAKE_RELWITHDEBINFOVK_POSTFIX drel)
    endif()
    
    # Force the configuration types to be used
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Configurations" FORCE)
    message(STATUS "ffx-api configurations set to: ${CMAKE_CONFIGURATION_TYPES}")
else()
    # Original behavior: standard configurations
    set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} /W3")
endif()

# set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo)
set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RELWITHDEBINFO_POSTFIX drel)

# Libs should be at another place if ffx-api is included in the solution
if(FFX_DEV_API)
    # Development mode: output to separate directory to avoid conflicts
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_HOME_DIRECTORY}/bin/ffx-api)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_HOME_DIRECTORY}/bin/ffx-api)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_HOME_DIRECTORY}/bin/ffx-api)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_HOME_DIRECTORY}/bin/ffx-api)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_HOME_DIRECTORY}/bin/ffx-api)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_HOME_DIRECTORY}/bin/ffx-api)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_HOME_DIRECTORY}/bin/ffx-api)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_HOME_DIRECTORY}/bin/ffx-api)
else()
    # Original behavior: output to main bin directory
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_HOME_DIRECTORY}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_HOME_DIRECTORY}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_HOME_DIRECTORY}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_HOME_DIRECTORY}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_HOME_DIRECTORY}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_HOME_DIRECTORY}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_HOME_DIRECTORY}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_HOME_DIRECTORY}/bin)
endif()



add_compile_definitions(FFXDLL_VERSION_MAJOR=1)
add_compile_definitions(FFXDLL_VERSION_MINOR=0)
add_compile_definitions(FFXDLL_VERSION_PATCH=1)
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

if (DEFINED ENV{CI_PIPELINE_ID})
add_compile_definitions(CI_PIPELINE_ID=$ENV{CI_PIPELINE_ID})
else()
add_compile_definitions(CI_PIPELINE_ID=0)
endif()

file(GLOB private_ffx_api
  ${CMAKE_CURRENT_SOURCE_DIR}/src/resource/*.*
	${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
	${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

file(GLOB public_ffx_api
	${CMAKE_CURRENT_SOURCE_DIR}/include/ffx_api/*.h
	${CMAKE_CURRENT_SOURCE_DIR}/include/ffx_api/*.hpp)

if (FFX_API_BACKEND STREQUAL VK_X64)
	message(WARNING "Setting FFX_BACKEND_VK")
	add_compile_definitions(FFXDLL_VERSION_API=VK)
	set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS FFX_BACKEND_VK)

	file(GLOB private_ffx_api_backend_vk
		${CMAKE_CURRENT_SOURCE_DIR}/src/vk/*.h
		${CMAKE_CURRENT_SOURCE_DIR}/src/vk/*.cpp)

	file(GLOB public_ffx_api_backend_vk
		${CMAKE_CURRENT_SOURCE_DIR}/include/ffx_api/vk/*.h
		${CMAKE_CURRENT_SOURCE_DIR}/include/ffx_api/vk/*.hpp)
endif()

if (FFX_API_BACKEND STREQUAL DX12_X64 OR
	FFX_API_BACKEND STREQUAL DX12_ARM64 OR
	FFX_API_BACKEND STREQUAL DX12_ARM64EC)
	message(WARNING "Setting FFX_BACKEND_DX12")
	add_compile_definitions(FFXDLL_VERSION_API=DX12)
	set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS FFX_BACKEND_DX12)

	file(GLOB private_ffx_api_backend_dx12
		${CMAKE_CURRENT_SOURCE_DIR}/src/dx12/*.h
		${CMAKE_CURRENT_SOURCE_DIR}/src/dx12/*.cpp)

	file(GLOB public_ffx_api_backend_dx12
		${CMAKE_CURRENT_SOURCE_DIR}/include/ffx_api/dx12/*.h
		${CMAKE_CURRENT_SOURCE_DIR}/include/ffx_api/dx12/*.hpp)
endif()

add_library(amd_fidelityfx_${FFX_PLATFORM_NAME} SHARED ${private_ffx_api} ${private_ffx_api_backend_dx12} ${private_ffx_api_backend_vk} ${public_ffx_api} ${public_ffx_api_backend_dx12} ${public_ffx_api_backend_vk})

source_group("private_source" FILES ${private_ffx_api})
source_group("public_source"  FILES ${public_ffx_api})

if (FFX_API_BACKEND STREQUAL VK_X64)
	source_group("private_source\\vk" FILES ${private_ffx_api_backend_vk})
	source_group("public_source\\vk"  FILES ${public_ffx_api_backend_vk})
endif()

if (FFX_API_BACKEND STREQUAL DX12_X64 OR
	FFX_API_BACKEND STREQUAL DX12_ARM64 OR
	FFX_API_BACKEND STREQUAL DX12_ARM64EC)
	source_group("private_source\\dx12" FILES ${private_ffx_api_backend_dx12})
	source_group("public_source\\dx12"  FILES ${public_ffx_api_backend_dx12})
endif()

target_include_directories(amd_fidelityfx_${FFX_PLATFORM_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(amd_fidelityfx_${FFX_PLATFORM_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include)

# FSR dependency, must be built before using BuildFidelityFXSDK.bat

if(NOT FFX_DEV_API)
    # Original behavior: build SDK as subdirectory when not in dev mode
    set(FFX_FSR2 ON)
    set(FFX_FSR3 ON)
    set(FFX_FSR3UPSCALER ON)
    set(FFX_OF ON)
    set(FFX_FI ON)
    set(FFX_AUTO_COMPILE_SHADERS ON)
    set(FFX_BUILD_AS_DLL OFF)
    add_subdirectory(../sdk sdk)
endif()
# In dev mode, we assume SDK is already built by the main solution

target_link_libraries(amd_fidelityfx_${FFX_PLATFORM_NAME} PRIVATE ffx_fsr2_${CMAKE_GENERATOR_PLATFORM} ffx_fsr3upscaler_${CMAKE_GENERATOR_PLATFORM} ffx_opticalflow_${CMAKE_GENERATOR_PLATFORM} ffx_frameinterpolation_${CMAKE_GENERATOR_PLATFORM} DXGI)
if (FFX_API_BACKEND STREQUAL VK_X64)
	find_package(Vulkan REQUIRED)
	target_link_libraries(amd_fidelityfx_${FFX_PLATFORM_NAME} PRIVATE Vulkan::Headers ffx_backend_vk_${CMAKE_GENERATOR_PLATFORM})
else() # DX12
	target_link_libraries(amd_fidelityfx_${FFX_PLATFORM_NAME} PRIVATE D3D12 ffx_backend_dx12_${CMAKE_GENERATOR_PLATFORM})
endif()
