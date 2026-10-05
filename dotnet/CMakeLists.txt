# C++/CLI mixed-mode assemblies for C# / VB.NET. The native libraries are linked in,
# so each DLL is all a .NET app needs (plus Ijwhost.dll for .NET 5+, copied by VS).
#   SoftEip.Net.dll       SoftFieldbus.EipAdapter        (EtherNet/IP only)
#   SoftFieldbus.Net.dll  SoftFieldbus.FieldbusDevice    (any transport, Hilscher-style API)

# /clr is incompatible with /RTC1 and needs /EHa instead of /EHsc. These are
# directory-scoped, so only targets in this folder are affected.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
endforeach()

if(SOFTEIP_DOTNET_FRAMEWORK MATCHES "^v[0-9]")
    string(REPLACE "v" "net" _cs_tfm "${SOFTEIP_DOTNET_FRAMEWORK}")
    string(REPLACE "." "" _cs_tfm "${_cs_tfm}")      # v4.8 -> net48
else()
    if(CMAKE_VERSION VERSION_LESS 3.26)
        message(FATAL_ERROR "C++/CLI for ${SOFTEIP_DOTNET_FRAMEWORK} needs CMake 3.26+ (VS 2026 ships a newer one)")
    endif()
    set(_cs_tfm ${SOFTEIP_DOTNET_FRAMEWORK})
endif()

# Common settings of a C++/CLI wrapper target.
function(softfb_clr_assembly target)
    target_compile_options(${target} PRIVATE /EHa /W3 /utf-8)
    set_target_properties(${target} PROPERTIES FOLDER "dotnet")
    if(SOFTEIP_DOTNET_FRAMEWORK MATCHES "^v[0-9]")
        # .NET Framework 4.x
        set_target_properties(${target} PROPERTIES
            COMMON_LANGUAGE_RUNTIME ""
            DOTNET_TARGET_FRAMEWORK_VERSION ${SOFTEIP_DOTNET_FRAMEWORK})
    else()
        # .NET (Core) 5+
        set_target_properties(${target} PROPERTIES
            COMMON_LANGUAGE_RUNTIME "netcore"
            DOTNET_TARGET_FRAMEWORK ${SOFTEIP_DOTNET_FRAMEWORK})
    endif()
endfunction()

add_library(SoftEip.Net SHARED SoftEipNet.h SoftEipNet.cpp)
target_link_libraries(SoftEip.Net PRIVATE softeip)
softfb_clr_assembly(SoftEip.Net)

add_library(SoftFieldbus.Net SHARED SoftFieldbusNet.h SoftFieldbusNet.cpp)
target_link_libraries(SoftFieldbus.Net PRIVATE softfieldbus)
softfb_clr_assembly(SoftFieldbus.Net)

# C# samples, generated into the build tree and added to the same solution.
option(SOFTEIP_DOTNET_SAMPLE "Add the C# sample projects to the solution" ON)
if(SOFTEIP_DOTNET_SAMPLE)
    set(SOFTEIP_CS_TFM ${_cs_tfm})
    set(SOFTEIP_BIN_DIR ${CMAKE_BINARY_DIR}/bin)

    set(SOFTEIP_SAMPLE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sample)
    configure_file(sample/SoftEipSample.csproj.in
                   ${CMAKE_CURRENT_BINARY_DIR}/sample/SoftEipSample.csproj @ONLY)
    include_external_msproject(SoftEipSample
        ${CMAKE_CURRENT_BINARY_DIR}/sample/SoftEipSample.csproj
        TYPE 9A19103F-16F7-4668-BE54-9A1E7A4F7556   # SDK-style C# project
        PLATFORM x64
        SoftEip.Net)
    set_target_properties(SoftEipSample PROPERTIES FOLDER "dotnet")

    set(SOFTFB_SAMPLE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/fieldbus_sample)
    configure_file(fieldbus_sample/SoftFieldbusSample.csproj.in
                   ${CMAKE_CURRENT_BINARY_DIR}/fieldbus_sample/SoftFieldbusSample.csproj @ONLY)
    include_external_msproject(SoftFieldbusSample
        ${CMAKE_CURRENT_BINARY_DIR}/fieldbus_sample/SoftFieldbusSample.csproj
        TYPE 9A19103F-16F7-4668-BE54-9A1E7A4F7556
        PLATFORM x64
        SoftFieldbus.Net)
    set_target_properties(SoftFieldbusSample PROPERTIES FOLDER "dotnet")
endif()
