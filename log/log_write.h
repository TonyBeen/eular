/*************************************************************************
    > File Name: log_write.h
    > Author: hsz
    > Desc: write log to file
    > Created Time: 2021年04月12日 星期一 22时16分35秒
 ************************************************************************/

#ifndef __LOG_WRITE_H__
#define __LOG_WRITE_H__

#include "log_event.h"
#include <string>
#include <error.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <memory>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>

#define MAX_FILE_SIZE       (5 * 1024 * 1024)  // one file' max size is 5mb
#define DEFAULT_NAME_SIZE   (64)

class NonCopyAndAssign
{
public:
    NonCopyAndAssign() {}
    NonCopyAndAssign(const NonCopyAndAssign&) = delete;
    NonCopyAndAssign &operator=(const NonCopyAndAssign&) = delete;
    virtual ~NonCopyAndAssign() {}
};

class ProcessMutex;

namespace eular {
class LogWrite : public NonCopyAndAssign {
public:
    LogWrite();
    virtual ~LogWrite();

    void setBasePath(const std::string &path)
    {
        if (path.length() == 0) {
            return;
        }

        mBasePath = path;
        if (mBasePath[mBasePath.length() - 1] != '/') {
            mBasePath.append("/");
        }
    }
    virtual int32_t      WriteToFile(std::string msg) = 0;
    virtual int32_t      WriteToFile(const LogEvent &ev) = 0;
    virtual std::string  getFileName() = 0;
    virtual uint32_t     getFileSize() = 0;
    virtual uint32_t     getFileMode() = 0;
    virtual bool         setFileMode(uint32_t mode) = 0;
    virtual uint32_t     getFileFlag() = 0;
    virtual bool         setFileFlag(uint32_t flag) = 0;

    virtual bool         CreateNewFile(std::string fileName) = 0;
    virtual bool         CloseFile() = 0;
    virtual uint16_t     type() const = 0;

protected:
    std::shared_ptr<ProcessMutex>   mMutex;     // 同步状态下保护文件描述符
    std::string                     mBasePath;
};

class StdoutLogWrite : public LogWrite {
public:
    StdoutLogWrite() {}
    ~StdoutLogWrite() {}

    virtual int32_t      WriteToFile(std::string msg) override;
    virtual int32_t      WriteToFile(const LogEvent &ev) override;
    virtual std::string  getFileName();
    virtual uint32_t     getFileSize();
    virtual uint32_t     getFileMode();
    virtual bool         setFileMode(uint32_t mode);
    virtual uint32_t     getFileFlag();
    virtual bool         setFileFlag(uint32_t flag);

    virtual bool         CreateNewFile(std::string fileName);
    virtual bool         CloseFile();
    virtual uint16_t     type() const { return static_cast<uint16_t>(OutputType::STDOUT); }

private:
    bool isInterrupt;
};

class FileLogWrite : public LogWrite {
public:
    FileLogWrite(uint32_t fileFlag = O_RDWR | O_CREAT | O_APPEND, uint32_t fileMode = 0664);
    virtual ~FileLogWrite();

    void                 maintainFile();
    virtual int32_t      WriteToFile(std::string msg) override;
    virtual int32_t      WriteToFile(const LogEvent &ev) override;
    virtual std::string  getFileName();
    virtual uint32_t     getFileSize();
    virtual uint32_t     getFileMode();
    virtual bool         setFileMode(uint32_t mode);
    virtual uint32_t     getFileFlag();
    virtual bool         setFileFlag(uint32_t flag);

    virtual bool         CreateNewFile(std::string fileName);
    virtual bool         CloseFile();
    virtual uint16_t     type() const { return static_cast<uint16_t>(OutputType::FILEOUT); }

private:
    bool        isInterrupt;
    int32_t*    mFileDesc;
    uint32_t    mFileMode;
    uint32_t    mFileFlag;
    uint64_t*   mFileSize;
};


class ConsoleLogWrite : public LogWrite
{
public:
    ConsoleLogWrite();
    ~ConsoleLogWrite();

    int32_t      WriteToFile(std::string msg) override;
    int32_t      WriteToFile(const LogEvent &ev) override;
    std::string  getFileName();
    uint32_t     getFileSize();
    uint32_t     getFileMode();
    bool         setFileMode(uint32_t mode);
    uint32_t     getFileFlag();
    bool         setFileFlag(uint32_t flag);

    bool         CreateNewFile(std::string fileName);
    bool         CloseFile();
    uint16_t     type() const { return static_cast<uint16_t>(OutputType::CONSOLEOUT); }

protected:
    void         InitParams();
    void         Destroy();
    static void  signalHandler(int32_t sig);        // 信号捕获处理函数

private:
    std::string         mLocalClientSockPath;
    std::string         mLocalServerSockPath;

    struct sockaddr_un  mServerSockAddr;
    struct sockaddr_un  mClientSockAddr;
    int32_t             mClientFd;
};

} // namespace eular
#endif // __LOG_WRITE_H__
