#include "log_format.h"

#define CLR_CLR         "\033[0m"       // 恢复颜色
#define CLR_BLACK       "\033[30m"      // 黑色字
#define CLR_RED         "\033[31m"      // 红色字
#define CLR_GREEN       "\033[32m"      // 绿色字
#define CLR_YELLOW      "\033[33m"      // 黄色字
#define CLR_BLUE        "\033[34m"      // 蓝色字
#define CLR_PURPLE      "\033[35m"      // 紫色字
#define CLR_SKYBLUE     "\033[36m"      // 天蓝字
#define CLR_WHITE       "\033[37m"      // 白色字

#define CLR_BLK_WHT     "\033[40;37m"   // 黑底白字
#define CLR_RED_WHT     "\033[41;37m"   // 红底白字
#define CLR_GREEN_WHT   "\033[42;37m"   // 绿底白字
#define CLR_YELLOW_WHT  "\033[43;37m"   // 黄底白字
#define CLR_BLUE_WHT    "\033[44;37m"   // 蓝底白字
#define CLR_PURPLE_WHT  "\033[45;37m"   // 紫底白字
#define CLR_SKYBLUE_WHT "\033[46;37m"   // 天蓝底白字
#define CLR_WHT_BLK     "\033[47;30m"   // 白底黑字

#define CLR_MAX_SIZE    (12)

#define COLOR_MAP(XXX)                          \
    XXX(LogLevel::UNKNOW,         CLR_CLR)      \
    XXX(LogLevel::LEVEL_DEBUG,    CLR_SKYBLUE)  \
    XXX(LogLevel::LEVEL_INFO,     CLR_GREEN)    \
    XXX(LogLevel::LEVEL_WARN,     CLR_YELLOW)   \
    XXX(LogLevel::LEVEL_ERROR,    CLR_RED)      \
    XXX(LogLevel::LEVEL_FATAL,    CLR_PURPLE)   \


namespace eular {
std::string LogFormat::Format(const LogEvent *ev)
{
    std::string ret;
    char output[PERFIX_SIZE] = {0};
    size_t msglen = strlen(ev->msg);
    ret.resize(msglen + PERFIX_SIZE + CLR_MAX_SIZE);
    const char *color = nullptr;
    ret = "";

#define XXX(level, clr) \
    case level:         \
        color = clr;    \
        break;          \

    switch (ev->level) {
        COLOR_MAP(XXX)

        default:
            break;
    }
#undef XXX

    if (ev->enableColor) {
        snprintf(output, PERFIX_SIZE, "%s", color);
        ret += output;
    }

    // time pid tid level tag:
    struct tm *pTime = localtime(&(ev->time.tv_sec));
    snprintf(output, PERFIX_SIZE, "%.2d-%.2d %.2d:%.2d:%.2d.%.3ld %5d %5ld %s %s: ",
        pTime->tm_mon + 1, pTime->tm_mday, pTime->tm_hour, pTime->tm_min, pTime->tm_sec, ev->time.tv_usec / 1000,
        ev->pid, ev->tid, LogLevel::ToFormatString(ev->level).c_str(), ev->tag);

    ret += output;
    ret += ev->msg;
    if (ev->msg[msglen - 1] != '\n') {
        ret += "\n";
    }

    // 清空颜色
    if (ev->enableColor) {
        snprintf(output, PERFIX_SIZE, "%s", CLR_CLR);
        ret += output;
    }

    return ret;
}

} // namespace eular
