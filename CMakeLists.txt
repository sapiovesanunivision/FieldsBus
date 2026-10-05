cmake_minimum_required(VERSION 3.16)
project(SoftFieldbus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SOFTEIP_SHARED "Build softeip as a native C++ DLL / shared library instead of a static lib" OFF)
if(MSVC)
    set(_dotnet_default ON)
else()
    set(_dotnet_default OFF)
endif()
option(SOFTEIP_BUILD_DOTNET "Build the C++/CLI .NET wrapper SoftEip.Net.dll (MSVC + Visual Studio generator)" ${_dotnet_default})
set(SOFTEIP_DOTNET_FRAMEWORK "net8.0" CACHE STRING
    "Wrapper target: net8.0 / net10.0 (.NET for WPF, WinUI, MAUI-Windows) or v4.8 (.NET Framework)")

# All executables and DLLs side by side: build/<preset>/bin/<Config>/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Threads REQUIRED)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Warning flags for our own native targets (kept PRIVATE so they don't leak into the /clr wrapper).
function(softeip_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /MP)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(SOFTEIP_HEADERS
    include/softeip/eip_adapter.hpp
    include/softeip/bytes.hpp
    include/softeip/cip_defs.hpp
    include/softeip/socket_compat.hpp
)
if(SOFTEIP_SHARED)
    add_library(softeip SHARED src/eip_adapter.cpp ${SOFTEIP_HEADERS})
    target_compile_definitions(softeip PUBLIC SOFTEIP_SHARED PRIVATE SOFTEIP_BUILDING_DLL)
else()
    add_library(softeip STATIC src/eip_adapter.cpp ${SOFTEIP_HEADERS})
endif()
source_group("Header Files" FILES ${SOFTEIP_HEADERS})
target_include_directories(softeip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(softeip PUBLIC Threads::Threads)
softeip_warnings(softeip)

if(WIN32)
    target_link_libraries(softeip PUBLIC ws2_32 winmm)
    target_compile_definitions(softeip PUBLIC _WIN32_WINNT=0x0601)
endif()

add_executable(eip_adapter_demo examples/eip_adapter_demo.cpp)
target_link_libraries(eip_adapter_demo PRIVATE softeip)
softeip_warnings(eip_adapter_demo)

add_executable(eip_scanner_sim tools/eip_scanner_sim.cpp)
target_link_libraries(eip_scanner_sim PRIVATE softeip)
softeip_warnings(eip_scanner_sim)

# ---- Modbus slave (TCP + UDP) ----------------------------------------------
add_library(softmb STATIC src/modbus_slave.cpp include/softmb/modbus_slave.hpp)
target_include_directories(softmb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(softmb PUBLIC Threads::Threads)
softeip_warnings(softmb)
if(WIN32)
    target_link_libraries(softmb PUBLIC ws2_32 winmm)
    target_compile_definitions(softmb PUBLIC _WIN32_WINNT=0x0601)
endif()

add_executable(mb_slave_demo examples/mb_slave_demo.cpp)
target_link_libraries(mb_slave_demo PRIVATE softmb)
softeip_warnings(mb_slave_demo)

add_executable(mb_master_sim tools/mb_master_sim.cpp)
target_link_libraries(mb_master_sim PRIVATE softmb)
softeip_warnings(mb_master_sim)

# Visual Studio solution niceties
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT eip_adapter_demo)
set_target_properties(softeip PROPERTIES FOLDER "native")
set_target_properties(eip_adapter_demo PROPERTIES FOLDER "examples")
set_target_properties(eip_scanner_sim PROPERTIES FOLDER "tools"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--target 127.0.0.1 --rpi-ms 10 --seconds 5 --local-port 2223")
set_target_properties(softmb PROPERTIES FOLDER "native")
set_target_properties(mb_slave_demo PROPERTIES FOLDER "examples")
set_target_properties(mb_master_sim PROPERTIES FOLDER "tools"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--target 127.0.0.1 --transport tcp")

if(SOFTEIP_BUILD_DOTNET)
    if(NOT MSVC OR NOT CMAKE_GENERATOR MATCHES "Visual Studio")
        message(WARNING "SOFTEIP_BUILD_DOTNET needs MSVC and a Visual Studio generator - skipping .NET wrapper")
    else()
        add_subdirectory(dotnet)
    endif()
endif()
