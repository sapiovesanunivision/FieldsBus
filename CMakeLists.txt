cmake_minimum_required(VERSION 3.16)
project(SoftFieldbus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SOFTEIP_SHARED "Build softeip as a native C++ DLL / shared library instead of a static lib" OFF)
option(SOFTFIELDBUS_SHARED "Also build SoftFieldbus.dll (softeip + softmb + softfb in one DLL, as in Dev.3); mb_client_test links it" OFF)
option(SOFTFIELDBUS_ANALYZE "MSVC: compile our native targets with /analyze /WX (catches what Dev.3's code analysis flags)" OFF)
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
        if(SOFTFIELDBUS_ANALYZE)
            target_compile_options(${target} PRIVATE /analyze /analyze:external- /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(SOFTEIP_HEADERS
    include/softeip/eip_adapter.hpp
    include/softeip/eip_scanner.hpp
    include/softeip/export.hpp
    include/softeip/bytes.hpp
    include/softeip/cip_defs.hpp
    include/softeip/socket_compat.hpp
)
if(SOFTEIP_SHARED)
    add_library(softeip SHARED src/eip_adapter.cpp src/eip_scanner.cpp ${SOFTEIP_HEADERS})
    target_compile_definitions(softeip PUBLIC SOFTEIP_SHARED PRIVATE SOFTEIP_BUILDING_DLL)
else()
    add_library(softeip STATIC src/eip_adapter.cpp src/eip_scanner.cpp ${SOFTEIP_HEADERS})
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

# ---- Modbus (softmb): SERVER (PC holds the registers) + CLIENT (PC polls a PLC) ----
add_library(softmb STATIC
    src/modbus_server.cpp src/modbus_client.cpp src/modbus_client_poller.cpp
    include/softmb/modbus_server.hpp include/softmb/modbus_slave.hpp include/softmb/modbus_defs.hpp
    include/softmb/modbus_client.hpp include/softmb/modbus_client_poller.hpp)
target_include_directories(softmb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(softmb PUBLIC Threads::Threads)
softeip_warnings(softmb)
if(WIN32)
    target_link_libraries(softmb PUBLIC ws2_32 winmm)
    target_compile_definitions(softmb PUBLIC _WIN32_WINNT=0x0601)
endif()

add_executable(mb_server_demo examples/mb_server_demo.cpp)
target_link_libraries(mb_server_demo PRIVATE softmb)
softeip_warnings(mb_server_demo)

add_executable(mb_client_test tools/mb_client_test.cpp)
softeip_warnings(mb_client_test)

add_executable(mb_client tools/mb_client.cpp)
target_link_libraries(mb_client PRIVATE softmb)
softeip_warnings(mb_client)

# ---- Transport-independent device (Hilscher-style process image) ------------
add_library(softfieldbus STATIC src/fieldbus_device.cpp include/softfb/fieldbus_device.hpp)
target_link_libraries(softfieldbus PUBLIC softeip softmb)
softeip_warnings(softfieldbus)

add_executable(fb_device_demo examples/fb_device_demo.cpp)
target_link_libraries(fb_device_demo PRIVATE softfieldbus)
softeip_warnings(fb_device_demo)

# ---- PROFINET RT device (softpn): raw Ethernet via the softpn_ndis driver or Npcap (Windows),
#      AF_PACKET (Linux) ----
set(SOFTPN_NPCAP_SDK "$ENV{NPCAP_SDK}" CACHE PATH "Npcap SDK folder (Include/, Lib/x64/); empty = only the softpn_ndis driver backend on Windows")
set(SOFTPN_ENABLED OFF)
set(SOFTPN_HAS_NPCAP OFF)
if(WIN32)
    set(SOFTPN_ENABLED ON)
    if(SOFTPN_NPCAP_SDK AND EXISTS "${SOFTPN_NPCAP_SDK}/Include/pcap.h")
        set(SOFTPN_HAS_NPCAP ON)
    else()
        message(STATUS "softpn: no Npcap SDK (SOFTPN_NPCAP_SDK), only the softpn_ndis driver backend")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SOFTPN_ENABLED ON)
endif()

# Windows raw-Ethernet backends for a target that compiles the softpn sources.
function(softpn_windows_l2 target scope)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/drivers/softpn_ndis/inc)
    target_link_libraries(${target} ${scope} iphlpapi)
    if(SOFTPN_HAS_NPCAP)
        target_compile_definitions(${target} PRIVATE SOFTPN_HAS_NPCAP=1)
        target_include_directories(${target} PRIVATE "${SOFTPN_NPCAP_SDK}/Include")
        target_link_libraries(${target} ${scope} "${SOFTPN_NPCAP_SDK}/Lib/x64/wpcap.lib" delayimp)
        # wpcap.dll is loaded on first use from System32\Npcap: no Npcap = error message, not a crash.
        if(scope STREQUAL "PUBLIC")
            target_link_options(${target} INTERFACE "/DELAYLOAD:wpcap.dll")
        else()
            target_link_options(${target} PRIVATE "/DELAYLOAD:wpcap.dll")
        endif()
    endif()
endfunction()

if(SOFTPN_ENABLED)
    set(SOFTPN_SOURCES
        src/pn/pn_frame.cpp
        src/pn/l2_port_memory.cpp
        src/pn/pn_station_store.cpp
        src/pn/pn_dcp.cpp
        src/pn/pn_lldp.cpp
        src/pn/pn_rpc.cpp
        src/pn/pn_blocks.cpp
        src/pn/pn_model.cpp
        src/pn/pn_cyclic.cpp
        src/pn/pn_rta.cpp
        src/pn/pn_ar.cpp
        src/pn/pn_device.cpp
    )
    if(WIN32)
        list(APPEND SOFTPN_SOURCES src/pn/l2_port_windows.cpp src/pn/l2_port_ndis.cpp)
        if(SOFTPN_HAS_NPCAP)
            list(APPEND SOFTPN_SOURCES src/pn/l2_port_npcap.cpp)
        endif()
    else()
        list(APPEND SOFTPN_SOURCES src/pn/l2_port_linux.cpp)
    endif()
    set(SOFTPN_HEADERS
        include/softpn/l2_port.hpp
        src/pn/pn_defs.hpp
        src/pn/pn_frame.hpp
        src/pn/pn_station_store.hpp
        src/pn/pn_dcp.hpp
        src/pn/pn_lldp.hpp
        src/pn/pn_uuid.hpp
        src/pn/pn_rpc.hpp
        src/pn/pn_blocks.hpp
        src/pn/pn_model.hpp
        src/pn/pn_cyclic.hpp
        src/pn/pn_rta.hpp
        src/pn/pn_ar.hpp
        include/softpn/pn_device.hpp
    )
    if(WIN32)
        list(APPEND SOFTPN_HEADERS src/pn/l2_port_windows.hpp drivers/softpn_ndis/inc/softpn_ndis_ioctl.h)
    endif()
    add_library(softpn STATIC ${SOFTPN_SOURCES} ${SOFTPN_HEADERS})
    source_group("Header Files" FILES ${SOFTPN_HEADERS})
    target_include_directories(softpn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pn)
    target_link_libraries(softpn PUBLIC softeip)
    softeip_warnings(softpn)
    if(WIN32)
        softpn_windows_l2(softpn PUBLIC)
    endif()
    set_target_properties(softpn PROPERTIES FOLDER "native")

    add_executable(pn_device_demo examples/pn_device_demo.cpp)
    target_link_libraries(pn_device_demo PRIVATE softpn)
    softeip_warnings(pn_device_demo)

    add_executable(pn_probe tools/pn_probe.cpp)
    target_include_directories(pn_probe PRIVATE src/pn)
    target_link_libraries(pn_probe PRIVATE softpn)
    softeip_warnings(pn_probe)

    # The controller simulator class is shared by the CLI tool and the offline self-test.
    add_library(pn_controller STATIC tools/pn_controller.cpp tools/pn_controller.hpp)
    target_include_directories(pn_controller PUBLIC src/pn tools)
    target_link_libraries(pn_controller PUBLIC softpn)
    softeip_warnings(pn_controller)

    add_executable(pn_controller_sim tools/pn_controller_sim.cpp)
    target_link_libraries(pn_controller_sim PRIVATE pn_controller)
    softeip_warnings(pn_controller_sim)

    add_executable(pn_selftest tools/pn_selftest.cpp)
    target_link_libraries(pn_selftest PRIVATE pn_controller)
    target_compile_definitions(pn_selftest PRIVATE
        SOFTPN_DEFAULT_GSDML="${CMAKE_CURRENT_SOURCE_DIR}/gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml")
    softeip_warnings(pn_selftest)
    enable_testing()
    add_test(NAME pn_selftest COMMAND pn_selftest)

    set_target_properties(pn_controller pn_probe pn_controller_sim pn_selftest PROPERTIES FOLDER "tools")
    set_target_properties(pn_device_demo PROPERTIES FOLDER "examples"
        VS_DEBUGGER_COMMAND_ARGUMENTS "--list")
    set_target_properties(pn_probe PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "--list")

    # PROFINET as a transport of the common API (softfb::FieldbusDevice, SoftFieldbus.Net).
    target_link_libraries(softfieldbus PUBLIC softpn)
    target_compile_definitions(softfieldbus PUBLIC SOFTFB_HAS_PROFINET=1)
endif()

# ---- Everything in one DLL: SoftFieldbus.dll (the Dev.3 SoftFieldbus*.dll is built from the same sources) ----
if(SOFTFIELDBUS_SHARED)
    add_library(softfieldbus_shared SHARED
        src/eip_adapter.cpp src/eip_scanner.cpp src/modbus_server.cpp src/modbus_client.cpp
        src/modbus_client_poller.cpp src/fieldbus_device.cpp)
    # Import library in its own folder: on Windows SoftFieldbus.lib and the static softfieldbus.lib are the same file name.
    set_target_properties(softfieldbus_shared PROPERTIES OUTPUT_NAME SoftFieldbus FOLDER "native"
                          ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/shared-lib
                          CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(softfieldbus_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(softfieldbus_shared PUBLIC SOFTFIELDBUS_SHARED PRIVATE SOFTFIELDBUS_BUILDING_DLL)
    target_link_libraries(softfieldbus_shared PUBLIC Threads::Threads)
    if(WIN32)
        target_link_libraries(softfieldbus_shared PUBLIC ws2_32 winmm)
        target_compile_definitions(softfieldbus_shared PUBLIC _WIN32_WINNT=0x0601)
    endif()
    if(SOFTPN_ENABLED)
        target_sources(softfieldbus_shared PRIVATE ${SOFTPN_SOURCES})
        target_include_directories(softfieldbus_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pn)
        target_compile_definitions(softfieldbus_shared PUBLIC SOFTFB_HAS_PROFINET=1)
        if(WIN32)
            softpn_windows_l2(softfieldbus_shared PRIVATE)
        endif()
    endif()
    softeip_warnings(softfieldbus_shared)
    target_link_libraries(mb_client_test PRIVATE softfieldbus_shared) # catches missing exports
else()
    target_link_libraries(mb_client_test PRIVATE softmb)
endif()

# Visual Studio solution niceties
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT eip_adapter_demo)
set_target_properties(softeip PROPERTIES FOLDER "native")
set_target_properties(eip_adapter_demo PROPERTIES FOLDER "examples")
set_target_properties(eip_scanner_sim PROPERTIES FOLDER "tools"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--target 127.0.0.1 --rpi-ms 10 --seconds 5 --local-port 2223")
set_target_properties(softmb softfieldbus PROPERTIES FOLDER "native")
set_target_properties(fb_device_demo PROPERTIES FOLDER "examples"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--transport modbus --unit 1")
set_target_properties(mb_server_demo PROPERTIES FOLDER "examples")
set_target_properties(mb_client PROPERTIES FOLDER "tools"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--host 127.0.0.1 read-holding 0 10")
set_target_properties(mb_client_test PROPERTIES FOLDER "tools"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--target 127.0.0.1 --transport tcp")

if(SOFTEIP_BUILD_DOTNET)
    if(NOT MSVC OR NOT CMAKE_GENERATOR MATCHES "Visual Studio")
        message(WARNING "SOFTEIP_BUILD_DOTNET needs MSVC and a Visual Studio generator - skipping .NET wrapper")
    else()
        add_subdirectory(dotnet)
    endif()
endif()
