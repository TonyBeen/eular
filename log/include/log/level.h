/*************************************************************************
    > File Name: level.h
    > Author: eular
    > Brief:
    > Created Time: Fri 24 Apr 2026 11:58:54 AM CST
 ************************************************************************/

#ifndef __LOG_LEVEL_H__
#define __LOG_LEVEL_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LEVEL_DEBUG     = 0,
    LEVEL_INFO      = 1,
    LEVEL_WARN      = 2,
    LEVEL_ERROR     = 3,
    LEVEL_FATAL     = 4,
    LEVEL_UNKNOWN   = 5,
} log_level_t;

typedef enum {
    STDOUT  = 0,
    FILEOUT = 1,
    UNKNOW  = 2,
} output_type_t;

#ifdef __cplusplus
}
#endif

#endif // __LOG_LEVEL_H__
