# SoftFieldbus SDK: imported targets for Windows x64 (Visual Studio 2026) and Linux x64 (GCC).
#
#   find_package(SoftFieldbus REQUIRED CONFIG PATHS <SDK folder>)
#
#   SoftFieldbus::shared        SoftFieldbus.dll / libSoftFieldbus.so: everything (EtherNet/IP, Modbus,
#                               FieldbusDevice, PROFINET) in one library. Recommended.
#   SoftFieldbus::softeip       static: EtherNet/IP adapter and scanner
#   SoftFieldbus::softmb        static: Modbus TCP/UDP server and client
#   SoftFieldbus::softpn        static: PROFINET RT device (Windows: also needs the Npcap SDK, see below)
#   SoftFieldbus::softfieldbus  static: FieldbusDevice, one API for all transports (links the three above)
#
# Windows: Debug and Release libraries (/MD, /MDd); RelWithDebInfo and MinSizeRel use Release.
# The static PROFINET libraries contain the Npcap backend (wpcap.dll, delay-loaded): to link them, set the
# environment variable NPCAP_SDK (or the cache variable SOFTFIELDBUS_NPCAP_SDK) to the Npcap SDK folder.
# Without it only SoftFieldbus::shared, ::softeip and ::softmb are defined. Running needs Npcap or the
# softpn_ndis driver either way (docs/softpn-l2-backend.md).

include(CMakeFindDependencyMacro)
find_dependency(Threads)

get_filename_component(_sfb_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(WIN32)
    set(_sfb_plat windows-x64)
else()
    set(_sfb_plat linux-x64)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT EXISTS "${_sfb_root}/lib/${_sfb_plat}")
    set(SoftFieldbus_FOUND FALSE)
    set(SoftFieldbus_NOT_FOUND_MESSAGE "SoftFieldbus SDK in ${_sfb_root} has no 64-bit libraries for ${_sfb_plat}")
    return()
endif()
set(_sfb_inc "${_sfb_root}/include")
set(_sfb_lib "${_sfb_root}/lib/${_sfb_plat}")

# ---- static libraries ----------------------------------------------------------------------------
function(_sfb_add_static name)
    if(TARGET SoftFieldbus::${name})
        return()
    endif()
    add_library(SoftFieldbus::${name} STATIC IMPORTED)
    if(WIN32)
        set_target_properties(SoftFieldbus::${name} PROPERTIES
            IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
            IMPORTED_LOCATION_DEBUG "${_sfb_lib}/Debug/${name}.lib"
            IMPORTED_LOCATION_RELEASE "${_sfb_lib}/Release/${name}.lib"
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
            MAP_IMPORTED_CONFIG_MINSIZEREL Release)
    else()
        set_target_properties(SoftFieldbus::${name} PROPERTIES IMPORTED_LOCATION "${_sfb_lib}/lib${name}.a")
    endif()
    set_target_properties(SoftFieldbus::${name} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_sfb_inc}")
endfunction()

_sfb_add_static(softeip)
_sfb_add_static(softmb)
foreach(_t softeip softmb)
    target_link_libraries(SoftFieldbus::${_t} INTERFACE Threads::Threads)
    if(WIN32)
        target_link_libraries(SoftFieldbus::${_t} INTERFACE ws2_32 winmm)
        target_compile_definitions(SoftFieldbus::${_t} INTERFACE _WIN32_WINNT=0x0601)
    endif()
endforeach()

set(_sfb_have_pn TRUE)
if(WIN32)
    if(NOT SOFTFIELDBUS_NPCAP_SDK)
        set(SOFTFIELDBUS_NPCAP_SDK "$ENV{NPCAP_SDK}" CACHE PATH "Npcap SDK folder, for the static PROFINET libraries")
    endif()
    if(NOT EXISTS "${SOFTFIELDBUS_NPCAP_SDK}/Lib/x64/wpcap.lib")
        set(_sfb_have_pn FALSE)
        if(NOT SoftFieldbus_FIND_QUIETLY)
            message(STATUS "SoftFieldbus: no Npcap SDK (NPCAP_SDK): static softpn / softfieldbus not available, "
                           "use SoftFieldbus::shared")
        endif()
    endif()
endif()
if(_sfb_have_pn)
    _sfb_add_static(softpn)
    target_link_libraries(SoftFieldbus::softpn INTERFACE SoftFieldbus::softeip)
    if(WIN32)
        target_link_libraries(SoftFieldbus::softpn INTERFACE iphlpapi "${SOFTFIELDBUS_NPCAP_SDK}/Lib/x64/wpcap.lib" delayimp)
        target_link_options(SoftFieldbus::softpn INTERFACE "/DELAYLOAD:wpcap.dll")
    endif()
    _sfb_add_static(softfieldbus)
    target_link_libraries(SoftFieldbus::softfieldbus INTERFACE SoftFieldbus::softpn SoftFieldbus::softmb SoftFieldbus::softeip)
endif()

# ---- shared library ------------------------------------------------------------------------------
if(NOT TARGET SoftFieldbus::shared)
    add_library(SoftFieldbus::shared SHARED IMPORTED)
    if(WIN32)
        set(_sfb_bin "${_sfb_root}/bin/${_sfb_plat}")
        set_target_properties(SoftFieldbus::shared PROPERTIES
            IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
            # own folder: Windows file names ignore case, SoftFieldbus.lib would replace the static softfieldbus.lib
            IMPORTED_IMPLIB_DEBUG "${_sfb_lib}/shared/Debug/SoftFieldbus.lib"
            IMPORTED_LOCATION_DEBUG "${_sfb_bin}/Debug/SoftFieldbus.dll"
            IMPORTED_IMPLIB_RELEASE "${_sfb_lib}/shared/Release/SoftFieldbus.lib"
            IMPORTED_LOCATION_RELEASE "${_sfb_bin}/Release/SoftFieldbus.dll"
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
            MAP_IMPORTED_CONFIG_MINSIZEREL Release)
        target_link_libraries(SoftFieldbus::shared INTERFACE ws2_32 winmm)
        target_compile_definitions(SoftFieldbus::shared INTERFACE _WIN32_WINNT=0x0601)
    else()
        set_target_properties(SoftFieldbus::shared PROPERTIES
            IMPORTED_LOCATION "${_sfb_lib}/libSoftFieldbus.so"
            IMPORTED_SONAME "libSoftFieldbus.so")
    endif()
    set_target_properties(SoftFieldbus::shared PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_sfb_inc}")
    target_compile_definitions(SoftFieldbus::shared INTERFACE SOFTFIELDBUS_SHARED)
    target_link_libraries(SoftFieldbus::shared INTERFACE Threads::Threads)
endif()

set(SoftFieldbus_FOUND TRUE)
