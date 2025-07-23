/*************************************************************************
    > File Name: utils.cpp
    > Author: hsz
    > Mail:
    > Created Time: Wed May  5 15:00:59 2021
 ************************************************************************/

// #include "utils/utils.h"
// #include "utils/errors.h"

// #include <string.h>
// #include <stdlib.h>
// #include <getopt.h>
// #include <assert.h>
// #include <pthread.h>
// #include <sys/types.h>
// #include <netinet/in.h> // for inet_ntoa
// #include <sys/socket.h>
// #include <pwd.h>
// #include <arpa/inet.h>
// #include <ifaddrs.h>    // for getifaddrs
// #include <chrono>

// ps也是走的这种方法
// #define BUF_SIZE 280
// std::vector<int> getPidByName(const char *procName)
// {
//     std::vector<int> pidVec;
//     if (procName == nullptr) {
//         return pidVec;
//     }
//     DIR *dir = nullptr;
//     struct dirent *ptr = nullptr;
//     FILE *fp = nullptr;
//     char fildPath[BUF_SIZE];
//     char cur_task_name[32];
//     UNUSED(cur_task_name);
//     char buf[BUF_SIZE];

//     dir = opendir("/proc");
//     if (nullptr != dir) {
//         // 循环读取/proc下的每一个文件
//         while ((ptr = readdir(dir)) != nullptr) {
//             // 如果读取到的是"."或者".."则跳过，读取到的不是文件夹类型也跳过
//             if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0))
//                 continue;
//             if (DT_DIR != ptr->d_type)
//                 continue;

//             // cmdlines存放的是启动进程时第一个入参，argv[0]
//             snprintf(fildPath, BUF_SIZE - 1,"/proc/%s/cmdline", ptr->d_name);
//             fp = fopen(fildPath, "r");
//             if (nullptr != fp) {
//                 if (fgets(buf, BUF_SIZE - 1, fp) == nullptr) {
//                     fclose(fp);
//                     continue;
//                 }

//                 // %*s %s表示添加了*的字符串会忽略，后面的%s赋值给cur_task_name
//                 // sscanf(buf, "%*s %s", cur_task_name); // 读取status时需要格式化字符串
//                 if (strcmp(procName, buf) == 0) {
//                     pidVec.push_back(atoi(ptr->d_name));
//                 }
//                 fclose(fp);
//             }
//         }
//         closedir(dir);
//     }
//     return pidVec;
// }

// std::string getNameByPid(pid_t pid)
// {
//     if (pid <= 0) {
//         return "";
//     }

//     FILE *fp = nullptr;
//     char buf[BUF_SIZE] = {0};

//     snprintf(buf, BUF_SIZE, "/proc/%d/status", pid);
//     fp = fopen(buf, "r");
//     if (fp == nullptr) {
//         perror("fopen failed:");
//         return "";
//     }
//     memset(buf, 0, BUF_SIZE);
//     fgets(buf, BUF_SIZE, fp);
//     if (buf[0] != '\0') {
//         char name[BUF_SIZE] = {0};
//         sscanf(buf, "%*s %s", name);
//         return name;
//     }
//     return "";
// }

// std::vector<std::string> getLocalAddress()
// {
//     static std::vector<std::string> ips;
//     struct ifaddrs *lo = nullptr;
//     struct ifaddrs *ifaddr = nullptr;

//     if (ips.size() == 0) {
//         getifaddrs(&ifaddr);
//         struct ifaddrs *root = ifaddr;  // need to free
//         while (ifaddr != nullptr) {
//             if (ifaddr->ifa_addr->sa_family == AF_INET) {   // IPv4
//                 if (std::string("lo") == ifaddr->ifa_name) {    // remove 127.0.0.1
//                     lo = ifaddr;
//                     ifaddr = ifaddr->ifa_next;
//                     continue;
//                 }
//                 in_addr *tmp = &((sockaddr_in *)ifaddr->ifa_addr)->sin_addr;
//                 ips.push_back(inet_ntoa(*tmp));
//             }
//             ifaddr = ifaddr->ifa_next;
//         }
//         freeifaddrs(root);
//     }

//     if (ips.size() == 0) {
//         std::vector<std::string> ret;
//         in_addr *tmp = &((sockaddr_in *)lo->ifa_addr)->sin_addr;
//         ret.push_back(inet_ntoa(*tmp));
//         return ret;
//     }
//     return ips;
// }


// std::vector<std::string> getdir(const std::string &path)
// {
//     DIR *dir = nullptr;
//     struct dirent *ptr = nullptr;
//     std::vector<std::string> ret;

//     dir = opendir(path.c_str());
//     if (dir != nullptr) {
//         while ((ptr = readdir(dir)) != nullptr) {
//             if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0))
//                 continue;

//             if (ptr->d_type == DT_REG) {
//                 ret.push_back(ptr->d_name);
//             }
//         }

//         closedir(dir);
//     }

//     return ret;
// }

// int32_t ForeachDir(const std::string &path, std::list<std::string> &fileList)
// {
//     DIR *dir = nullptr;
//     struct dirent *ptr = nullptr;

//     int32_t count = 0;
//     dir = opendir(path.c_str());
//     if (dir != nullptr) {
//         while ((ptr = readdir(dir)) != nullptr) {
//             if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0)) {
//                 continue;
//             }

//             if (ptr->d_type == DT_REG) {
//                 fileList.push_back(path + "/" + ptr->d_name);
//                 ++count;
//             } else if (ptr->d_type == DT_DIR) {
//                 count += ForeachDir(path + "/" + ptr->d_name, fileList);
//             }
//         }

//         closedir(dir);
//     }

//     return count;
// }
