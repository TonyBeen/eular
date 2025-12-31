/*************************************************************************
    > File Name: debug.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年11月21日 星期四 15时15分42秒
 ************************************************************************/

#include "utils/debug.h"

void log2stdout(const char *fileName, int32_t line, const char *format, ...)
{
    std::string output = fileName;
    va_list args;
    va_start(args, format);
    char messageBuffer[1024];
    vsnprintf(messageBuffer, sizeof(messageBuffer), format, args);
    va_end(args);

    output.push_back(':');
    output += std::to_string(line);
    output.append(": ");
    output.append(messageBuffer);
    if (output.back() == '\n') {
        printf("%s", output.c_str());
    } else {
        printf("%s\n", output.c_str());
    }
}
