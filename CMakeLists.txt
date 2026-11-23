cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name: also the firmware file name (iRotor.elf/.bin/.hex).
# The CubeMX project file keeps its own name (Single_board_FW_1.0.ioc); that only matters to CubeMX.
set(CMAKE_PROJECT_NAME iRotor)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

get_filename_component(TOOLCHAIN_BIN_DIR ${CMAKE_C_COMPILER} DIRECTORY)
find_program(ARM_OBJCOPY
    NAMES ${CMAKE_OBJCOPY} arm-none-eabi-objcopy
    HINTS ${TOOLCHAIN_BIN_DIR}
)
if(NOT ARM_OBJCOPY)
    message(FATAL_ERROR "arm-none-eabi-objcopy not found")
endif()

# Enable CMake support for ASM, C and C++ languages
enable_language(C CXX ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/azimuth_service.c
    Core/Src/fw_info.c
    Core/Src/fw_update.c
    Core/Src/position_service.c
    Core/Src/pwm_service.c
    Core/Src/settings_service.c
    Core/Src/uart_commands.c
    Core/Src/web_pages.cpp
    Core/Src/web_ui_gz.c
    Core/Src/w5500.c
)

# The azimuth web UI (web_ui/index.html) is embedded gzipped. Regenerate it when the page changes;
# without Python the committed Core/Src/web_ui_gz.c is used as is.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${CMAKE_SOURCE_DIR}/Core/Src/web_ui_gz.c
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/build_web_ui.py
        DEPENDS ${CMAKE_SOURCE_DIR}/web_ui/index.html ${CMAKE_SOURCE_DIR}/tools/build_web_ui.py
        COMMENT "Packing web_ui/index.html into web_ui_gz.c"
    )
endif()

# Firmware version from the git tag (vMAJOR.MINOR.PATCH), regenerated on every build.
# Only fw_info.c includes the header, so a new build time recompiles one small file.
set(FW_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/fw_version.h)
add_custom_target(fw_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${FW_VERSION_HEADER}
            -P ${CMAKE_SOURCE_DIR}/tools/fw_version.cmake
    BYPRODUCTS ${FW_VERSION_HEADER}
    COMMENT "Reading firmware version from git"
    VERBATIM
)
add_dependencies(${CMAKE_PROJECT_NAME} fw_version)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_BINARY_DIR}/generated
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${ARM_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.hex
    COMMAND ${ARM_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin
    # Release files with the version in the name + SHA256SUMS in <build>/release/
    COMMAND ${CMAKE_COMMAND} -DBIN_DIR=$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}> -DNAME=${CMAKE_PROJECT_NAME}
            -DVERSION_HEADER=${FW_VERSION_HEADER} -P ${CMAKE_SOURCE_DIR}/tools/package_release.cmake
    COMMENT "Generating HEX and BIN firmware images"
)
