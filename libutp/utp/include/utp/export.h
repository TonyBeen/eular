#ifndef EULAR_UTP_EXPORT_H
#define EULAR_UTP_EXPORT_H

/*
 * The API is usable from both static and shared builds.  UTP_BUILD is
 * defined only while compiling the library itself; consumers of a Windows
 * DLL therefore get dllimport automatically.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#    if defined(UTP_STATIC)
#        define UTP_API
#    elif defined(UTP_BUILD)
#        define UTP_API __declspec(dllexport)
#    else
#        define UTP_API __declspec(dllimport)
#    endif
#elif defined(__GNUC__) || defined(__clang__)
#    define UTP_API __attribute__((visibility("default")))
#else
#    define UTP_API
#endif

#endif  // EULAR_UTP_EXPORT_H
