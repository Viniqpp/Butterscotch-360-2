# PLATFORM=xbox360-xdk: CMake front end for the Xbox 360 XDK build.
#
# The XDK 21256.3 Xenon compiler is driven through its Visual Studio 2010
# MSBuild "Xbox 360" platform, so this delegates to tools/xdk/build-xdk.ps1,
# which builds Butterscotch360.vcxproj. The .vcxproj stays the single source
# of truth for defines, include paths, libraries and XEX packaging.
#
#   cmake -S . -B build-xdk -DPLATFORM=xbox360-xdk [-DXDK_DIR=...] [-DXDK_CONFIGURATION=Debug]
#   cmake --build build-xdk                       # Butterscotch XEX
#   cmake --build build-xdk --target xdk_smoke_xex  # smoke-test XEX

if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "PLATFORM=xbox360-xdk needs a Windows host with Visual Studio 2010 and XDK 21256.3.")
endif()

set(XDK_DIR "$ENV{XEDK}" CACHE PATH "Xbox 360 XDK root (defaults to the XEDK environment variable)")
set(XDK_CONFIGURATION "Release" CACHE STRING "XDK configuration: Release or Debug")
set_property(CACHE XDK_CONFIGURATION PROPERTY STRINGS Release Debug)

find_program(XDK_POWERSHELL NAMES powershell pwsh REQUIRED)
set(_xdk_script "${CMAKE_CURRENT_SOURCE_DIR}/tools/xdk/build-xdk.ps1")
set(_xdk_args -NoProfile -ExecutionPolicy Bypass -File "${_xdk_script}" -Configuration "${XDK_CONFIGURATION}")
if(XDK_DIR)
    list(APPEND _xdk_args -XdkDir "${XDK_DIR}")
endif()

# Fail at configure time with the script's itemised list of what is missing.
execute_process(COMMAND "${XDK_POWERSHELL}" ${_xdk_args} -CheckOnly RESULT_VARIABLE _xdk_check)
if(NOT _xdk_check EQUAL 0)
    message(FATAL_ERROR "Xbox 360 XDK environment check failed (see the list above).")
endif()

add_custom_target(butterscotch_xex ALL
    COMMAND "${XDK_POWERSHELL}" ${_xdk_args} -Target butterscotch -Package
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building Butterscotch XEX with the Xbox 360 XDK")

add_custom_target(xdk_smoke_xex
    COMMAND "${XDK_POWERSHELL}" ${_xdk_args} -Target smoke -Package
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building the XDK smoke-test XEX")
