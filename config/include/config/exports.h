/*************************************************************************
    > File Name: exports.h
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Mar 2026 02:58:47 PM CST
 ************************************************************************/

#ifndef __CONFIG_EXPORTS_H__
#define __CONFIG_EXPORTS_H__

#if defined(_MSC_VER)
    #if defined(CONFIG_STATIC)
        #define CONFIG_API
    #elif defined(CONFIG_EXPORTS)
        #define CONFIG_API __declspec(dllexport)
    #else
        #define CONFIG_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    #if defined(CONFIG_STATIC)
        #define CONFIG_API
    #else
        #define CONFIG_API __attribute__((visibility("default")))
    #endif
#else
    #define CONFIG_API
#endif

#endif // __CONFIG_EXPORTS_H__
