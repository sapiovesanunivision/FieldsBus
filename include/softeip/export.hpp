// Export macros for building the libraries as DLLs / shared libraries.
//
//   static libs (CMake default)  nothing defined           -> SOFTEIP_API etc. are empty
//   softeip alone as a DLL       SOFTEIP_SHARED (+ SOFTEIP_BUILDING_DLL while building it)
//   everything in one DLL        SOFTFIELDBUS_SHARED (+ SOFTFIELDBUS_BUILDING_DLL while building it)
//                                CMake option SOFTFIELDBUS_SHARED, Dev.3 SoftFieldbus*.dll
#pragma once

#if defined(SOFTFIELDBUS_SHARED)
#ifndef SOFTEIP_SHARED
#define SOFTEIP_SHARED
#endif
#ifndef SOFTMB_SHARED
#define SOFTMB_SHARED
#endif
#ifndef SOFTFB_SHARED
#define SOFTFB_SHARED
#endif
#endif

#if defined(SOFTFIELDBUS_BUILDING_DLL)
#ifndef SOFTEIP_BUILDING_DLL
#define SOFTEIP_BUILDING_DLL
#endif
#ifndef SOFTMB_BUILDING_DLL
#define SOFTMB_BUILDING_DLL
#endif
#ifndef SOFTFB_BUILDING_DLL
#define SOFTFB_BUILDING_DLL
#endif
#endif

#if defined(_WIN32)
#define SOFTFIELDBUS_DETAIL_EXPORT __declspec(dllexport)
#define SOFTFIELDBUS_DETAIL_IMPORT __declspec(dllimport)
#else
#define SOFTFIELDBUS_DETAIL_EXPORT __attribute__((visibility("default")))
#define SOFTFIELDBUS_DETAIL_IMPORT
#endif

#if !defined(SOFTEIP_API)
#if defined(SOFTEIP_SHARED) && defined(SOFTEIP_BUILDING_DLL)
#define SOFTEIP_API SOFTFIELDBUS_DETAIL_EXPORT
#elif defined(SOFTEIP_SHARED)
#define SOFTEIP_API SOFTFIELDBUS_DETAIL_IMPORT
#else
#define SOFTEIP_API
#endif
#endif

#if !defined(SOFTMB_API)
#if defined(SOFTMB_SHARED) && defined(SOFTMB_BUILDING_DLL)
#define SOFTMB_API SOFTFIELDBUS_DETAIL_EXPORT
#elif defined(SOFTMB_SHARED)
#define SOFTMB_API SOFTFIELDBUS_DETAIL_IMPORT
#else
#define SOFTMB_API
#endif
#endif

#if !defined(SOFTFB_API)
#if defined(SOFTFB_SHARED) && defined(SOFTFB_BUILDING_DLL)
#define SOFTFB_API SOFTFIELDBUS_DETAIL_EXPORT
#elif defined(SOFTFB_SHARED)
#define SOFTFB_API SOFTFIELDBUS_DETAIL_IMPORT
#else
#define SOFTFB_API
#endif
#endif
