# SoftFieldbus SDK samples: Windows x64 (Visual Studio 2026) and Linux x64 (GCC).
#
#   Windows:  cmake --preset vs2026           then  cmake --build --preset vs2026-release
#   Linux:    cmake --preset linux            then  cmake --build --preset linux
#   static:   add -DSOFTFIELDBUS_USE_SHARED=OFF to the configure step (Windows: needs NPCAP_SDK for PROFINET)
#
# Your own project: copy the find_package() and target_link_libraries() lines.
cmake_minimum_required(VERSION 3.21)
project(SoftFieldbusExamples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SOFTFIELDBUS_USE_SHARED "Link SoftFieldbus.dll / libSoftFieldbus.so instead of the static libraries" ON)
set(SOFTFIELDBUS_SDK "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "SoftFieldbus SDK folder (contains cmake/, include/, lib/)")
find_package(SoftFieldbus REQUIRED CONFIG PATHS "${SOFTFIELDBUS_SDK}/cmake" NO_DEFAULT_PATH)

# sample            static library it needs
set(SAMPLES
    eip_adapter_demo  softeip       # EtherNet/IP adapter (I/O assemblies)
    mb_server_demo    softmb        # Modbus TCP/UDP server
    fb_device_demo    softfieldbus  # one API, any transport (eip, modbus, profinet)
    pn_device_demo    softpn)       # PROFINET RT device

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
while(SAMPLES)
    list(POP_FRONT SAMPLES sample lib)
    if(NOT SOFTFIELDBUS_USE_SHARED AND NOT TARGET SoftFieldbus::${lib})
        message(STATUS "${sample}: skipped (SoftFieldbus::${lib} not available, see cmake/SoftFieldbusConfig.cmake)")
        continue()
    endif()
    add_executable(${sample} ${sample}.cpp)
    if(MSVC)
        target_compile_options(${sample} PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(${sample} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(SOFTFIELDBUS_USE_SHARED)
        target_link_libraries(${sample} PRIVATE SoftFieldbus::shared)
        if(WIN32) # SoftFieldbus.dll next to the program
            add_custom_command(TARGET ${sample} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${sample}> $<TARGET_FILE_DIR:${sample}>
                COMMAND_EXPAND_LISTS)
        endif()
    else()
        target_link_libraries(${sample} PRIVATE SoftFieldbus::${lib})
    endif()
endwhile()
