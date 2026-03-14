/*************************************************************************
    > File Name: exports.h
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Mar 2026 02:58:47 PM CST
 ************************************************************************/

#ifndef __CONFIG_EXPORTS_H__
#define __CONFIG_EXPORTS_H__

#include <utils/sysdef.h>

#if COMPILER_TYPE == COMPILER_MSVC
    #ifndef CONFIG_STATIC
        #ifdef CONFIG_EXPORTS
            #define CONFIG_API __declspec(dllexport)
        #else
            #define CONFIG_API __declspec(dllimport)
        #endif
    #else
        #define CONFIG_API
    #endif
#elif COMPILER_TYPE == COMPILER_GNUC || COMPILER_TYPE == COMPILER_CLANG || COMPILER_TYPE == COMPILER_APPLECLANG
    #ifndef CONFIG_STATIC
        #define CONFIG_API __attribute__((visibility("default")))
    #else
        #define CONFIG_API
    #endif
#else
    #error "Unknown compiler type."
#endif

#endif // __CONFIG_EXPORTS_H__
