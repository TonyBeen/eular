/*************************************************************************
    > File Name: utils.h
    > Author: hsz
    > Mail:
    > Created Time: Wed May  5 14:55:59 2021
 ************************************************************************/

#ifndef __UTILS_FUNCTION_H__
#define __UTILS_FUNCTION_H__

#include <utils/sysdef.h>

#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <list>
#include <unordered_map>

#include <utils/string8.h>

#ifndef gettid
#define gettid() syscall(__NR_gettid)
#endif

#ifdef DISALLOW_COPY_AND_ASSIGN
#undef DISALLOW_COPY_AND_ASSIGN
#endif
#define DISALLOW_COPY_AND_ASSIGN(ClassName) \
    ClassName(const ClassName&) = delete; \
    ClassName& operator=(const ClassName&) = delete;

#ifdef __cplusplus
    #define EXTERN_C_BEGIN extern "C" {
    #define EXTERN_C_END }
    #define DEFAULT(x) = x
#else
    #define EXTERN_C_BEGIN
    #define EXTERN_C_END
    #define DEFAULT(x)
#endif

#define eular_likely(cond)          __builtin_expect(!!(cond), 1)       // 编译器优化，条件大概率成立
#define eular_unlikely(cond)        __builtin_expect(!!(cond), 0)       // 编译器优化，条件大概率不成立

#define eular_atomic_or(P, V)       __sync_or_and_fetch((P), (V))       // p: 地址 V: 值，P指向的内容与V相或
#define eular_atomic_and(P, V)      __sync_and_and_fetch((P), (V))
#define eular_atomic_add(P, V)      __sync_add_and_fetch((P), (V))      // 前置++
#define eular_atomic_load(P)        __sync_add_and_fetch((P), (0))
#define eular_atomic_xadd(P, V)     __sync_fetch_and_add((P), (V))      // 后置++

// P: 地址 O: 旧值 N: 新值; if (O == *P) { *p = N; return O} else { return *P }
#define cmpxchg(P, O, N)            __sync_val_compare_and_swap((P), (O), (N))

#if defined(__x86_64__)
#define CPU_RELAX_NOP()             asm volatile("rep; nop\n": : :"memory")
#define CPU_RELAX_PAUSE()           asm volatile("pause" ::: "memory")
#else
#define CPU_RELAX_NOP()
#define CPU_RELAX_PAUSE()
#endif

#ifndef UNUSED
#define UNUSED(x) (void)x;
#endif

typedef unsigned long long nsec_t;
nsec_t  seconds(uint16_t sec);
nsec_t  mseconds(uint16_t ms);
bool    Mkdir(const std::string &path);
int32_t GetFileLength(const eular::String8 &path);
int     msleep(uint32_t ms);

std::vector<int>    getPidByName(const char *procName);
std::string         getNameByPid(pid_t pid);
std::vector<std::string> getLocalAddress();
std::vector<std::string> getdir(const std::string &path);
int32_t ForeachDir(const std::string &path, std::list<std::string> &fileList);

bool isPicture(const std::string &fileName);

std::string Time2Str(time_t ts, const std::string& format = "%Y-%m-%d %H:%M:%S");
time_t Str2Time(const char* str, const char* format = "%Y-%m-%d %H:%M:%S");

class TypeUtil {
public:
    static int8_t   ToChar(const std::string& str);
    static int64_t  Atoi(const std::string& str);
    static double   Atof(const std::string& str);
    static int8_t   ToChar(const char* str);
    static int64_t  Atoi(const char* str);
    static double   Atof(const char* str);
};

std::unordered_map<std::string, std::string> getargopt(int argc, char **argv, const char *opt);

namespace Time {
/**
 * @brief 获取系统时间(等价于clock_gettime->CLOCK_REALTIME)
 * 
 * @return uint64_t 返回毫秒
 */
uint64_t SystemTime();

/**
 * @brief 获取绝对时间(等价于clock_gettime->CLOCK_MONOTONIC)
 * 
 * @return uint64_t 返回毫秒
 */
uint64_t Abstime(bool useAsm = false);

/**
 * @brief 格式化为格林尼治时间 如: Thu, 17 Mar 2022 01:53:47 GMT，与北京时间差8个小时
 * 
 * @param buf 输出位置
 * @param buflen 缓存大小
 * @param tim 如果不为null则使用tim格式化，否则获取当前时间在格式化
 * @return true 
 * @return false 
 */
bool gmttime(char *buf, size_t buflen, const time_t *tim = nullptr);

} // namespace time

#endif // __UTILS_FUNCTION_H__
