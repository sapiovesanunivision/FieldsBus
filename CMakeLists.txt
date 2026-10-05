cmake_minimum_required(VERSION 3.16)
project(SoftFieldbus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(softeip STATIC src/eip_adapter.cpp)
target_include_directories(softeip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(softeip PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(softeip PUBLIC ws2_32 winmm)
    target_compile_definitions(softeip PUBLIC _WIN32_WINNT=0x0601)
endif()

if(MSVC)
    target_compile_options(softeip PUBLIC /W4 /utf-8)
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
