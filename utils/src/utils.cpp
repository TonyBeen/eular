/*************************************************************************
    > File Name: utils.cpp
    > Author: hsz
    > Mail:
    > Created Time: Wed May  5 15:00:59 2021
 ************************************************************************/

#include "utils/utils.h"
#include "utils/errors.h"

#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>
#include <pthread.h>
#include <sys/types.h>
#include <netinet/in.h> // for inet_ntoa
#include <sys/socket.h>
#include <pwd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>    // for getifaddrs
#include <chrono>

nsec_t seconds(uint16_t sec)
{
    return sec * 1000 * 1000 * 1000;
}

nsec_t mseconds(uint16_t ms)
{
    return ms * 1000 * 1000;
}

bool __lstat(const char *path)
{
    struct stat lst;
    int ret = lstat(path, &lst);
    return ret;
}

bool __mkdir(const char *path)
{
    if(access(path, F_OK) == 0) {
        return true;
    }

    return 0 == mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

bool Mkdir(const std::string &path)
{
    if(__lstat(path.c_str()) == 0) {
        return true;
    }
    std::string realPath = path;
    if (path[0] == '~') {
        uid_t uid = getuid();
        struct passwd *p = getpwuid(uid);
        if (p != nullptr) {
            realPath = p->pw_dir;
            realPath.append(path.c_str() + 1);
        }
    }
    char* filePath = strdup(realPath.c_str());
    char* ptr = strchr(filePath + 1, '/');
    do {
        for(; ptr; *ptr = '/', ptr = strchr(ptr + 1, '/')) {
            *ptr = '\0';
            if(__mkdir(filePath) != 0) {
                break;
            }
        }
        if(ptr != nullptr) {
            break;
        } else if(!__mkdir(filePath)) {
            break;
        }
        free(filePath);
        return true;
    } while(0);
    free(filePath);
    return false;
}

int32_t GetFileLength(const eular::String8 &path)
{
    static struct stat lst;
    int ret = stat(path.c_str(), &lst);
    if (ret != 0) {
        return Status::NOT_FOUND;
    }
    return static_cast<int32_t>(lst.st_size);
}

int msleep(uint32_t ms)
{
    struct timeval time;
    int second = ms / 1000;
    int us = (ms - second * 1000) * 1000;
    time.tv_sec = ms / 1000;
    time.tv_usec = us;

    return select(0, NULL, NULL, NULL, &time);
}

// ps也是走的这种方法
#define BUF_SIZE 280
std::vector<int> getPidByName(const char *procName)
{
    std::vector<int> pidVec;
    if (procName == nullptr) {
        return pidVec;
    }
    DIR *dir = nullptr;
    struct dirent *ptr = nullptr;
    FILE *fp = nullptr;
    char fildPath[BUF_SIZE];
    char cur_task_name[32];
    UNUSED(cur_task_name);
    char buf[BUF_SIZE];

    dir = opendir("/proc");
    if (nullptr != dir) {
        // 循环读取/proc下的每一个文件
        while ((ptr = readdir(dir)) != nullptr) {
            // 如果读取到的是"."或者".."则跳过，读取到的不是文件夹类型也跳过
            if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0))
                continue;
            if (DT_DIR != ptr->d_type)
                continue;

            // cmdlines存放的是启动进程时第一个入参，argv[0]
            snprintf(fildPath, BUF_SIZE - 1,"/proc/%s/cmdline", ptr->d_name);
            fp = fopen(fildPath, "r");
            if (nullptr != fp) {
                if (fgets(buf, BUF_SIZE - 1, fp) == nullptr) {
                    fclose(fp);
                    continue;
                }

                // %*s %s表示添加了*的字符串会忽略，后面的%s赋值给cur_task_name
                // sscanf(buf, "%*s %s", cur_task_name); // 读取status时需要格式化字符串
                if (strcmp(procName, buf) == 0) {
                    pidVec.push_back(atoi(ptr->d_name));
                }
                fclose(fp);
            }
        }
        closedir(dir);
    }
    return pidVec;
}

std::string getNameByPid(pid_t pid)
{
    if (pid <= 0) {
        return "";
    }

    FILE *fp = nullptr;
    char buf[BUF_SIZE] = {0};

    snprintf(buf, BUF_SIZE, "/proc/%d/status", pid);
    fp = fopen(buf, "r");
    if (fp == nullptr) {
        perror("fopen failed:");
        return "";
    }
    memset(buf, 0, BUF_SIZE);
    fgets(buf, BUF_SIZE, fp);
    if (buf[0] != '\0') {
        char name[BUF_SIZE] = {0};
        sscanf(buf, "%*s %s", name);
        return name;
    }
    return "";
}

std::vector<std::string> getLocalAddress()
{
    static std::vector<std::string> ips;
    struct ifaddrs *lo = nullptr;
    struct ifaddrs *ifaddr = nullptr;

    if (ips.size() == 0) {
        getifaddrs(&ifaddr);
        struct ifaddrs *root = ifaddr;  // need to free
        while (ifaddr != nullptr) {
            if (ifaddr->ifa_addr->sa_family == AF_INET) {   // IPv4
                if (std::string("lo") == ifaddr->ifa_name) {    // remove 127.0.0.1
                    lo = ifaddr;
                    ifaddr = ifaddr->ifa_next;
                    continue;
                }
                in_addr *tmp = &((sockaddr_in *)ifaddr->ifa_addr)->sin_addr;
                ips.push_back(inet_ntoa(*tmp));
            }
            ifaddr = ifaddr->ifa_next;
        }
        freeifaddrs(root);
    }

    if (ips.size() == 0) {
        std::vector<std::string> ret;
        in_addr *tmp = &((sockaddr_in *)lo->ifa_addr)->sin_addr;
        ret.push_back(inet_ntoa(*tmp));
        return ret;
    }
    return ips;
}

bool isPicture(const std::string &fileName)
{
    static const char *extentArray[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico"
    };

    size_t size = sizeof(extentArray) / sizeof(char *);
    for (size_t i = 0; i < size; ++i) {
        if (fileName.find_first_of(extentArray[i])) {
            return true;
        }
    }

    return false;
}

std::vector<std::string> getdir(const std::string &path)
{
    DIR *dir = nullptr;
    struct dirent *ptr = nullptr;
    std::vector<std::string> ret;

    dir = opendir(path.c_str());
    if (dir != nullptr) {
        while ((ptr = readdir(dir)) != nullptr) {
            if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0))
                continue;

            if (ptr->d_type == DT_REG) {
                ret.push_back(ptr->d_name);
            }
        }

        closedir(dir);
    }

    return ret;
}

int32_t ForeachDir(const std::string &path, std::list<std::string> &fileList)
{
    DIR *dir = nullptr;
    struct dirent *ptr = nullptr;

    int32_t count = 0;
    dir = opendir(path.c_str());
    if (dir != nullptr) {
        while ((ptr = readdir(dir)) != nullptr) {
            if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0)) {
                continue;
            }

            if (ptr->d_type == DT_REG) {
                fileList.push_back(path + "/" + ptr->d_name);
                ++count;
            } else if (ptr->d_type == DT_DIR) {
                count += ForeachDir(path + "/" + ptr->d_name, fileList);
            }
        }

        closedir(dir);
    }

    return count;
}

pid_t GetThreadId()
{
    return syscall(__NR_gettid);
}

std::string Time2Str(time_t ts, const std::string& format)
{
    struct tm tm;
    localtime_r(&ts, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), format.c_str(), &tm);
    return buf;
}

time_t Str2Time(const char* str, const char* format)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    if(!strptime(str, format, &t)) {
        return 0;
    }
    return mktime(&t);
}

int8_t  TypeUtil::ToChar(const std::string& str)
{
    if(str.empty()) {
        return 0;
    }
    return *str.begin();
}

int64_t TypeUtil::Atoi(const std::string& str)
{
    if(str.empty()) {
        return 0;
    }
    return strtoull(str.c_str(), nullptr, 10);
}

double  TypeUtil::Atof(const std::string& str)
{
    if(str.empty()) {
        return 0;
    }
    return atof(str.c_str());
}

int8_t  TypeUtil::ToChar(const char* str)
{
    if(str == nullptr) {
        return 0;
    }
    return str[0];
}

int64_t TypeUtil::Atoi(const char* str)
{
    if(str == nullptr) {
        return 0;
    }
    return strtoull(str, nullptr, 10);
}

double  TypeUtil::Atof(const char* str)
{
    if(str == nullptr) {
        return 0;
    }
    return atof(str);
}

std::unordered_map<std::string, std::string> getargopt(int argc, char **argv, const char *opt)
{
    assert(argc > 0);
    std::unordered_map<std::string, std::string> result;
    if (!opt || !argv) {
        return result;
    }

    std::string temp(opt);
    if (temp.length() == 0) {
        return result;
    }

    std::vector<uint8_t> characterVec;
    uint8_t charMap[128] = {0};
    for (uint8_t it : temp) {
        if ( ('a' <= it && it <= 'z') || ('A' <= it || it <= 'Z')) {
            charMap[it] = 1;
        }
    }

    char c = '\0';
    while ((c = ::getopt(argc, argv, opt)) > 0) {
        uint8_t index = static_cast<uint8_t>(c);
        if (charMap[index]) {
            result.emplace(eular::String8::Format("-%c", c).c_str(), optarg ? optarg : "");
            //result.insert(std::make_pair(eular::String8::Format("-%c", c).c_str(), optarg ? optarg : "null"));
        }
    }

    return result;
}

namespace Time {
uint64_t SystemTime()
{
    std::chrono::system_clock::time_point tm = std::chrono::system_clock::now();
    std::chrono::milliseconds mills = 
        std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
    return mills.count();
}

static unsigned long long __one_msec;
static unsigned long long __one_sec;
static unsigned long long __metric_diff = 0;
static pthread_once_t __once_control2 = PTHREAD_ONCE_INIT;

static inline unsigned long long rte_rdtsc(void)
{
    union {
        unsigned long long tsc_64;
        #if BYTE_ORDER == LITTLE_ENDIAN
        struct {
            unsigned lo_32;
            unsigned hi_32;
        };
        #elif BYTE_ORDER == BIG_ENDIAN
        struct {
            unsigned hi_32;
            unsigned lo_32;
        };
        #endif
    } tsc;

    asm volatile("rdtsc" :
            "=a" (tsc.lo_32),
            "=d" (tsc.hi_32));
    return tsc.tsc_64;
}

void set_time_metric()
{
    unsigned long long now, startup, end;
    unsigned long long begin = rte_rdtsc();
    usleep(1000);
    end        = rte_rdtsc();
    __one_msec = end - begin;
    __one_sec  = __one_msec * 1000;     // 获取CPU频率

    startup    = rte_rdtsc();           // 获取当前cpu时间戳
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    now        = tp.tv_sec * __one_sec; // 获取系统绝对时间
    // now = time(NULL) * __one_sec;    // 获取系统实时时间
    if (now > startup) {
        __metric_diff = now - startup;
    } else {
        __metric_diff = 0;
    }
}

uint64_t asm_gettimeofday()
{
    if (__metric_diff == 0) {
        if (pthread_once(&__once_control2, set_time_metric) != 0) {
            abort();
        }
    }

    uint64_t now = rte_rdtsc() + __metric_diff;
    return now / __one_sec + (now % __one_sec) / __one_msec;
}

uint64_t Abstime(bool useAsm)
{
    if (useAsm) {
        return asm_gettimeofday();
    }

    std::chrono::steady_clock::time_point tm = std::chrono::steady_clock::now();
    std::chrono::milliseconds mills = 
        std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
    return mills.count();
}

bool gmttime(char *buf, size_t buflen, const time_t *tim)
{
    time_t t = time(nullptr);
    if (tim != nullptr) {
        t = *tim;
    }
    tm stm;
    memset(&stm, 0, sizeof(stm));
    gmtime_r(&t, &stm);
    return strftime(buf, buflen, "%a, %d %b %Y %H:%M:%S GMT", &stm) > 0;
}
} // namespace time
