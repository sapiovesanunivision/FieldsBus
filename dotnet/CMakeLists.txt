# SoftEip.Net.dll - C++/CLI mixed-mode assembly exposing softeip to C# / VB.NET.
# The native library is linked in, so this one DLL is all a .NET app needs
# (plus Ijwhost.dll for .NET 5+, which Visual Studio copies automatically).

# /clr is incompatible with /RTC1 and needs /EHa instead of /EHsc. These are
# directory-scoped, so only targets in this folder are affected.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
endforeach()

add_library(SoftEip.Net SHARED SoftEipNet.h SoftEipNet.cpp)
target_link_libraries(SoftEip.Net PRIVATE softeip)
target_compile_options(SoftEip.Net PRIVATE /EHa /W3 /utf-8)
set_target_properties(SoftEip.Net PROPERTIES FOLDER "dotnet")

if(SOFTEIP_DOTNET_FRAMEWORK MATCHES "^v[0-9]")
    # .NET Framework 4.x
    set_target_properties(SoftEip.Net PROPERTIES
        COMMON_LANGUAGE_RUNTIME ""
        DOTNET_TARGET_FRAMEWORK_VERSION ${SOFTEIP_DOTNET_FRAMEWORK})
    string(REPLACE "v" "net" _cs_tfm "${SOFTEIP_DOTNET_FRAMEWORK}")
    string(REPLACE "." "" _cs_tfm "${_cs_tfm}")      # v4.8 -> net48
else()
    # .NET (Core) 5+
    if(CMAKE_VERSION VERSION_LESS 3.26)
        message(FATAL_ERROR "C++/CLI for ${SOFTEIP_DOTNET_FRAMEWORK} needs CMake 3.26+ (VS 2026 ships a newer one)")
    endif()
    set_target_properties(SoftEip.Net PROPERTIES
        COMMON_LANGUAGE_RUNTIME "netcore"
        DOTNET_TARGET_FRAMEWORK ${SOFTEIP_DOTNET_FRAMEWORK})
    set(_cs_tfm ${SOFTEIP_DOTNET_FRAMEWORK})
endif()

# C# sample, generated into the build tree and added to the same solution.
option(SOFTEIP_DOTNET_SAMPLE "Add the C# sample project to the solution" ON)
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
endif()
