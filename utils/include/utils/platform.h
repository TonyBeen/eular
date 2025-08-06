/*************************************************************************
    > File Name: platform.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年08月06日 星期三 19时53分43秒
 ************************************************************************/

#ifndef __UTILS_PLATFORM_H__
#define __UTILS_PLATFORM_H__

#include <utils/sysdef.h>

#if defined(OS_WINDOWS)
    #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
    #elif _WIN32_WINNT < 0x0600
    #undef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef _CRT_NONSTDC_NO_DEPRECATE
    #define _CRT_NONSTDC_NO_DEPRECATE
    #endif
    #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
    #endif
    #ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
    #define _WINSOCK_DEPRECATED_NO_WARNINGS
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>   // for inet_pton,inet_ntop
    #include <windows.h>
    #include <process.h>    // for getpid,exec
    #include <direct.h>     // for mkdir,rmdir,chdir,getcwd
    #include <io.h>         // for open,close,read,write,lseek,tell

    #define eular_sleep(s)     Sleep((s) * 1000)
    #define eular_msleep(ms)   Sleep(ms)
    #define eular_usleep(us)   Sleep((us) / 1000)

    // access
    #ifndef F_OK
    #define F_OK            0       /* test for existence of file */
    #endif
    #ifndef X_OK
    #define X_OK            (1<<0)  /* test for execute or search permission */
    #endif
    #ifndef W_OK
    #define W_OK            (1<<1)  /* test for write permission */
    #endif
    #ifndef R_OK
    #define R_OK            (1<<2)  /* test for read permission */
    #endif

    // stat
    #ifndef S_ISREG
    #define S_ISREG(st_mode) (((st_mode) & S_IFMT) == S_IFREG)
    #endif
    #ifndef S_ISDIR
    #define S_ISDIR(st_mode) (((st_mode) & S_IFMT) == S_IFDIR)
    #endif
#else
    #include <unistd.h>
    #include <dirent.h>     // for mkdir,rmdir,chdir,getcwd

    // socket
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <netinet/udp.h>
    #include <netdb.h>  // for gethostbyname

    #define eular_sleep(s)     sleep(s)
    #define eular_msleep(ms)   usleep((ms) * 1000)
    #define eular_usleep(us)   usleep(us)
#endif

#if COMPILER_TYPE == COMPILER_MSVC
    typedef int pid_t;
    typedef int gid_t;
    typedef int uid_t;
    #define strcasecmp  stricmp
    #define strncasecmp strnicmp
#else
    typedef int                 BOOL;
    typedef unsigned char       BYTE;
    typedef unsigned short      WORD;
    typedef unsigned int        DWORD;
    typedef void*               HANDLE;
    #include <strings.h>
    #define stricmp             strcasecmp
    #define strnicmp            strncasecmp
#endif

#endif // __UTILS_PLATFORM_H__