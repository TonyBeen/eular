/*************************************************************************
    > File Name: debug.h
    > Author: hsz
    > Mail:
    > Created Time: Thu 29 Jul 2021 11:02:40 PM PDT
 ************************************************************************/
#ifndef __DEBUG_H__
#define __DEBUG_H__

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

#include <string>

#include <utils/sysdef.h>

extern "C" UTILS_API void log2stdout(const char *fileName, int32_t line, const char *format, ...);
#ifdef _DEBUG
    #ifndef LOG
        #define LOG(format, ...) log2stdout(__FILE_NAME__, __LINE__, format, ##__VA_ARGS__);
    #endif
#else
    #ifndef LOG
        #define LOG(...)
    #endif
#endif

#endif