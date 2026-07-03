#include "log_write.h"
#include "mutex.hpp"
#include "log_format.h"
#include "callstack.h"
#include "nlohmann/json.hpp"
#include <assert.h>
#include <string>
#include <sstream>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/mman.h>

namespace eular {
LogWrite::LogWrite()
{
    mMutex = std::make_shared<ProcessMutex>();
}

LogWrite::~LogWrite()
{
}

int32_t StdoutLogWrite::WriteToFile(std::string msg)
{
    int32_t ret = 0;
    if (msg.length()) {
        AutoLock<ProcessMutex> lock(mMutex);
        ret = write(STDOUT_FILENO, msg.c_str(), msg.length());
    }

    return ret;
}

int32_t StdoutLogWrite::WriteToFile(const LogEvent &ev)
{
    const std::string &format_string = LogFormat::Format(&ev);
    int32_t ret = format_string.length();
    if (ret > 0) {
        AutoLock<ProcessMutex> lock(mMutex);;
        ret = ::write(STDOUT_FILENO, format_string.c_str(), ret);
    }

    return ret;
}

std::string StdoutLogWrite::getFileName()
{
    return std::string("stdout");
}

uint32_t StdoutLogWrite::getFileSize()
{
    return 0;
}

uint32_t StdoutLogWrite::getFileMode()
{
    return O_WRONLY;
}

bool StdoutLogWrite::setFileMode(uint32_t mode)
{
    (void)mode;
    return true;
}

uint32_t StdoutLogWrite::getFileFlag()
{
    return 0;
}

bool StdoutLogWrite::setFileFlag(uint32_t flag)
{
    (void)flag;
    return true;
}

bool StdoutLogWrite::CreateNewFile(std::string fileName)
{
    (void)fileName;
    return true;
}

bool StdoutLogWrite::CloseFile()
{
    return true;
}

FileLogWrite::FileLogWrite(uint32_t fileFlag, uint32_t fileMode) :
    mFileMode(fileMode),
    mFileFlag(fileFlag)
{
    mFileDesc = (int32_t *)mmap(nullptr, sizeof(int32_t),
        PROT_WRITE|PROT_READ, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    mFileSize = (uint64_t *)mmap(nullptr, sizeof(uint64_t), 
        PROT_WRITE|PROT_READ, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    assert(mFileDesc && mFileSize);
}

FileLogWrite::~FileLogWrite()
{
    CloseFile();
    munmap(mFileDesc, sizeof(int32_t));
    munmap(mFileSize, sizeof(uint64_t));
}

std::string FileLogWrite::getFileName()
{
    time_t curr = time(nullptr);
    struct tm *p = localtime(&curr);

    char buf[1024] = {0};
    sprintf(buf, "log-%.4d%.2d%.2d-%.2d%.2d%.2d.log",
        1900 + p->tm_year,
        1 + p->tm_mon,
        p->tm_mday,
        p->tm_hour,
        p->tm_min,
        p->tm_sec);
    return std::string(buf);
}

void FileLogWrite::maintainFile()
{
    AutoLock<ProcessMutex> lock(mMutex);
    if (*mFileDesc <= 0 || *mFileSize > MAX_FILE_SIZE) {
        CloseFile();
        CreateNewFile(getFileName());
    }
}

int32_t FileLogWrite::WriteToFile(std::string msg)
{
    int32_t ret = 0;
    maintainFile();
    AutoLock<ProcessMutex> lock(mMutex);
    if (msg.length()) {
        ret = write(*mFileDesc, msg.c_str(), msg.length());
    }
    if (ret > 0) {
        *mFileSize += ret;
    } else if (ret < 0) {
        perror("write error");
    }
    return ret;
}

int32_t FileLogWrite::WriteToFile(const LogEvent &ev)
{
    const std::string &format_string = LogFormat::Format(&ev);
    int32_t ret = format_string.length();
    if (ret > 0) {
        maintainFile();
        AutoLock<ProcessMutex> lock(mMutex);
        ret = write(*mFileDesc, format_string.c_str(), format_string.length());
        if (ret > 0) {
            *mFileSize += ret;
        } else if (ret < 0) {
            perror("write error");
        }
    }

    return ret;
}

uint32_t FileLogWrite::getFileSize()
{
    return *mFileSize;
}

uint32_t FileLogWrite::getFileMode()
{
    return mFileMode;
}
bool FileLogWrite::setFileMode(uint32_t mode)
{
    mFileMode = mode;
    return true;
}
uint32_t FileLogWrite::getFileFlag()
{
    return mFileFlag;
}
bool FileLogWrite::setFileFlag(uint32_t flag)
{
    mFileFlag = flag;
    return true;
}

static int32_t __lstat(const char *path)
{
    struct stat lst;
    int32_t ret = lstat(path, &lst);
    return ret;
}

static bool __mkdir(const char *path)
{
    if (access(path, F_OK) == 0) {
        return 0;
    }
    return mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

bool Mkdir(const std::string &path)
{
    if (__lstat(path.c_str()) == 0) {
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
        } else if(__mkdir(filePath) != 0) {
            break;
        }
        free(filePath);
        return true;
    } while(0);
    free(filePath);
    return false;
}

bool FileLogWrite::CreateNewFile(std::string fileName)
{
    if (*mFileDesc > 0 && *mFileSize < MAX_FILE_SIZE) {
        return true;
    }

    std::string path = "~/log/";
    if (mBasePath.length()) {
        path = mBasePath;
    }

    std::string realPath = path;
    if (path[0] == '~') {
        uid_t uid = getuid();
        struct passwd *p = getpwuid(uid);
        if (p != nullptr) {
            // printf("\tname = %s\n", p->pw_name);
            // printf("\tpasswd = %s", p->pw_passwd);
            // printf("\tuid = %u", p->pw_uid);
            // printf("\tuid = %u", p->pw_gid);
            // printf("\tdir = %s", p->pw_dir);
            // printf("\tshell = %s", p->pw_shell);
            realPath = p->pw_dir;
            realPath.append(path.c_str() + 1);
        }
    }
    if (!Mkdir(path)) {
        return false;
    }
    realPath += fileName;

    *mFileDesc = open(realPath.c_str(), mFileFlag, mFileMode);
    if (*mFileDesc < 0) {
        printf("open file (%s) error: %s\n", realPath.c_str(), strerror(errno));
        return false;
    }
    *mFileSize = 0;
    return true;
}

bool FileLogWrite::CloseFile()
{
    *mFileSize = 0;
    if (*mFileDesc <= 0) {
        return true;
    }
    return !close(*mFileDesc);
}

#define LOCAL_SOCKET_SERVER_PATH        "/tmp/log_sock_server"
#define LOCAL_SOCKET_CLIENT_PATH_FMT    "/tmp/log_sock_client_%d"   // %d 是父进程的ID，使用子进程ID时多进程时容易出问题
#define MAX_SIZE_OF_SUNPATH             108
static std::string gClientSockPath;
ConsoleLogWrite *pConsoleLogWrite = nullptr;

ConsoleLogWrite::ConsoleLogWrite() :
    mClientFd(-1)
{
    mClientSockAddr.sun_family = AF_LOCAL;
    snprintf(mClientSockAddr.sun_path, MAX_SIZE_OF_SUNPATH, LOCAL_SOCKET_CLIENT_PATH_FMT, getppid());
    mLocalClientSockPath = mClientSockAddr.sun_path;
    gClientSockPath = mLocalClientSockPath;

    mServerSockAddr.sun_family = AF_LOCAL;
    snprintf(mServerSockAddr.sun_path, MAX_SIZE_OF_SUNPATH, LOCAL_SOCKET_SERVER_PATH);
    mLocalServerSockPath = mServerSockAddr.sun_path;

    // 捕获信号
    signal(SIGINT, signalHandler);
    signal(SIGQUIT, signalHandler);
    signal(SIGSEGV, signalHandler);
    signal(SIGABRT, signalHandler);
    signal(SIGPIPE, SIG_IGN);

    InitParams();
    pConsoleLogWrite = this;
}

ConsoleLogWrite::~ConsoleLogWrite()
{
    Destroy();
    pConsoleLogWrite = nullptr;
}

void ConsoleLogWrite::InitParams()
{
    if (mClientFd > 0) {
        return;
    }

    mClientFd = ::socket(AF_LOCAL, SOCK_STREAM, 0);
    if (mClientFd < 0) {
        perror("socket error:");
        return;
    }

    int flags = fcntl(mClientFd, F_GETFL,0);
    if (-1 == fcntl(mClientFd, F_SETFL, flags | O_NONBLOCK)) {
        perror("Set socket unblock failed!");
    }

    do {
        int32_t nRetCode = ::bind(mClientFd, (const sockaddr *)&mClientSockAddr, sizeof(mClientSockAddr));
        if (nRetCode < 0) {
            break;
        }
        nRetCode = ::connect(mClientFd, (sockaddr *)&mServerSockAddr, sizeof(mServerSockAddr));
        if (nRetCode < 0) {
            if (errno != EINPROGRESS) {
                printf("[%s %d] Connect fail!\n",__FILE__,__LINE__);
                break;
            }

            fd_set rset, wset;
            FD_ZERO(&rset);
            FD_SET(mClientFd, &rset);
            wset = rset;
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200 * 1000;
            if (select(mClientFd + 1, &rset, &wset, NULL, &tv) == 0) {
                printf("%s() select failed!\n", __func__);
                break;
            }
        }

        const char *jsonLevel = "{  \
        \"id\": \"notice\",         \
        \"keywords\": \"level\",    \
        \"level\": [                \
            { \"key\": \"DEBUG\", \"type\": \"int\", \"value\": 0 }, \
            { \"key\": \"INFO\",  \"type\": \"int\", \"value\": 1 }, \
            { \"key\": \"WARN\",  \"type\": \"int\", \"value\": 2 }, \
            { \"key\": \"ERROR\", \"type\": \"int\", \"value\": 3 }, \
            { \"key\": \"FATAL\", \"type\": \"int\", \"value\": 4 }, \
            { \"key\": \"UNKNOW\", \"type\": \"int\", \"value\": -1 }\
        ]}\r\n\r\n";

        ::send(mClientFd, jsonLevel, strlen(jsonLevel), 0);
        return;
    } while(0);

    Destroy();
}

void ConsoleLogWrite::Destroy()
{
    if (mClientFd > 0) {
        close(mClientFd);
        mClientFd = -1;
    }
    unlink(mLocalClientSockPath.c_str());
}

#define tostr(code) #code

void ConsoleLogWrite::signalHandler(int32_t sig)
{
    const char *pSignal = nullptr;
    switch (sig) {
    case SIGINT:
        pSignal = tostr(SIGINT);
        break;
    case SIGQUIT:
        pSignal = tostr(SIGQUIT);
        break;
    case SIGSEGV:
        pSignal = tostr(SIGSEGV);
        break;
    case SIGABRT:
        pSignal = tostr(SIGABRT);
        break;
    default:
        pSignal = "UNKNOW";
        break;
    }
    char log[128] = {0};
    sprintf(log, "%s() catch signal %s\n", __func__, pSignal);
    pConsoleLogWrite->WriteToFile(log);
    CallStack stack;
    stack.update();
    if (sig == SIGSEGV || sig == SIGABRT) {
        pConsoleLogWrite->WriteToFile(stack.toString());
    }
    unlink(gClientSockPath.c_str());
    _exit(0);
}

int32_t ConsoleLogWrite::WriteToFile(std::string msg)
{
    AutoLock<ProcessMutex> lock(mMutex);
    InitParams();

    int32_t sendSize = 0;
    if (mClientFd > 0) {
        sendSize = ::send(mClientFd, msg.c_str(), msg.length(), 0);
        if (sendSize <= 0) { // 服务端不在线
            Destroy();
        }
    }

    return sendSize;
}

int32_t ConsoleLogWrite::WriteToFile(const LogEvent &ev)
{
    uint64_t milliSecond = ev.time.tv_sec * 1000 + ev.time.tv_usec / 1000;

    nlohmann::json logJsonObj;
    logJsonObj["id"] = "data";
    logJsonObj["time"] = milliSecond;
    logJsonObj["pid"] = ev.pid;
    logJsonObj["tid"] = ev.tid;
    logJsonObj["level"] = ev.level;
    logJsonObj["tag"] = ev.tag;
    logJsonObj["msg"] = ev.msg;

    std::stringstream ss;
    ss << logJsonObj;
    std::string msg = ss.str();
    msg.append("\r\n\r\n");

    AutoLock<ProcessMutex> lock(mMutex);
    InitParams();

    int32_t sendSize = 0;
    if (mClientFd > 0) {
        sendSize = ::send(mClientFd, msg.c_str(), msg.length(), 0);
        if (sendSize <= 0) { // 服务端不在线
            Destroy();
        }
    }

    return 0;
}

std::string ConsoleLogWrite::getFileName()
{
    return mLocalClientSockPath;
}

uint32_t ConsoleLogWrite::getFileSize()
{
    return 0;
}

uint32_t ConsoleLogWrite::getFileMode()
{
    return 0;
}

bool ConsoleLogWrite::setFileMode(uint32_t mode)
{
    (void)mode;
    return true;
}

uint32_t ConsoleLogWrite::getFileFlag()
{
    return 0;
}

bool ConsoleLogWrite::setFileFlag(uint32_t flag)
{
    (void)flag;
    return true;
}

bool ConsoleLogWrite::CreateNewFile(std::string fileName)
{
    (void)fileName;
    return true;
}

bool ConsoleLogWrite::CloseFile()
{
    return true;
}

}
