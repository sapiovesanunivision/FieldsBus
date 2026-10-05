cmake_minimum_required(VERSION 3.16)
project(SoftFieldbus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(SOFTEIP_HEADERS
    include/softeip/eip_adapter.hpp
    include/softeip/bytes.hpp
    include/softeip/cip_defs.hpp
    include/softeip/socket_compat.hpp
)
add_library(softeip STATIC src/eip_adapter.cpp ${SOFTEIP_HEADERS})
source_group("Header Files" FILES ${SOFTEIP_HEADERS})
target_include_directories(softeip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(softeip PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(softeip PUBLIC ws2_32 winmm)
    target_compile_definitions(softeip PUBLIC _WIN32_WINNT=0x0601)
endif()

if(MSVC)
    target_compile_options(softeip PUBLIC /W4 /utf-8 /permissive- /MP)
else()
    target_compile_options(softeip PUBLIC -Wall -Wextra -Wpedantic)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/eip_adapter_demo.cpp)
    add_executable(eip_adapter_demo examples/eip_adapter_demo.cpp)
    target_link_libraries(eip_adapter_demo PRIVATE softeip)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/eip_scanner_sim.cpp)
    add_executable(eip_scanner_sim tools/eip_scanner_sim.cpp)
    target_link_libraries(eip_scanner_sim PRIVATE softeip)
endif()

# Visual Studio solution niceties
if(TARGET eip_adapter_demo)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT eip_adapter_demo)
    set_target_properties(eip_adapter_demo PROPERTIES FOLDER "examples")
endif()
if(TARGET eip_scanner_sim)
    set_target_properties(eip_scanner_sim PROPERTIES FOLDER "tools"
        VS_DEBUGGER_COMMAND_ARGUMENTS "--target 127.0.0.1 --rpi-ms 10 --seconds 5 --local-port 2223")
endif()
