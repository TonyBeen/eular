/*************************************************************************
    > File Name: log_level.h
    > Author: hsz
    > Desc: This file is for log level.
    > Created Time: 2021年04月12日 星期一 22时16分04秒
 ************************************************************************/

#ifndef __LOG_LOG_LEVEL_H__
#define __LOG_LOG_LEVEL_H__

#include <log/log.h>

#include <string>
#include <string.h>

namespace eular {

enum class OutputType {
    STDOUT = 0,
    FILEOUT = 1,
    CONSOLEOUT = 2,
    UNKNOW
};
} // namespace eular
#endif // #ifndef __LOG_LEVEL_H__