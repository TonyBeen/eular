#include "utils/file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <stdexcept>

#include "utils/code_convert.h"
#include "utils/debug.h"
#include "utils/errors.h"

#define INVLID_FILE_DESCRIPTOR (-1)

#if defined(OS_WINDOWS)
#include <io.h>
#include <Windows.h>
// file attribute
#define EU_O_RDONLY     _O_RDONLY           // 只读
#define EU_O_WRONLY     _O_WRONLY           // 只写
#define EU_O_RDWR       _O_RDWR             // 读写
#define EU_O_CREAT      _O_CREAT            // 创建
#define EU_O_TRUNC      _O_TRUNC            // 使文件截取为0(需有写入权限)
#define EU_O_APPEND     _O_APPEND           // 追加
#define EU_O_BINARY     _O_BINARY           // 二进制打开，默认是文本方式
#define EU_O_EXCL       _O_EXCL             // 打开文件的原子操作(当指定的文件存在时，则返回一个错误值)
#define EU_O_TMPFILE    _O_TEMPORARY        // 创建一个文件作为临时文件，在关闭最后一个文件描述符时，删除该文件

#define EU_MODE        (_S_IREAD|_S_IWRITE) // 允许读写(open的第三个参数)

// file operator
#define EU_OPEN         ::_wopen
#define EU_READ         ::_read
#define EU_WRITE        ::_write
#define EU_CLOSE        ::_close
#define EU_STATF        ::_wstat
#define EU_SEEK         ::_lseek

#define ST_MTIME        st_mtime
#define ST_CTIME        st_ctime

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif // !CP_UTF8

// std::wstring UTF8_To_Unicode(const std::string& strIn)
// {
//     if (strIn.empty())
//     {
//         return std::wstring();
//     }

//     std::wstring result;
//     try
//     {
//         int32_t len = MultiByteToWideChar(CP_UTF8, 0, strIn.c_str(), -1, nullptr, 0);
//         if (len > 0)
//         {
//             result.reserve((uint32_t)len);
//             result.resize(uint32_t(len - 1), 0); // 第四个参数传入-1时返回值包含末尾的一个\0终止字符
//             wchar_t* wszUnicode = &result[0];
//             MultiByteToWideChar(CP_UTF8, 0, strIn.c_str(), (int32_t)strIn.length(), wszUnicode, len);
//         }
//     }
//     catch (...)
//     {
//         result = std::wstring();
//     }

//     return r
// }


// std::string Unicode_To_UTF8(const std::wstring& strIn)
// {
//     if (strIn.empty())
//     {
//         return std::string();
//     }

//     std::string result;
//     try
//     {
//         int32_t len = WideCharToMultiByte(CP_UTF8, 0, strIn.c_str(), -1, nullptr, 0, nullptr, nullptr);
//         if (len > 0)
//         {
//             result.reserve((uint32_t)len);
//             result.resize(uint32_t(len - 1), 0);
//             char* szUtf8 = &result[0];
//             WideCharToMultiByte(CP_UTF8, 0, strIn.c_str(), (int32_t)result.length(), szUtf8, len, nullptr, nullptr);
//         }
//     }
//     catch (...)
//     {
//         result = std::string();
//     }

//     return result;
// }

#elif defined(OS_LINUX)
#include <unistd.h>
#define EU_O_RDONLY     O_RDONLY
#define EU_O_WRONLY     O_WRONLY
#define EU_O_RDWR       O_RDWR
#define EU_O_CREAT      O_CREAT
#define EU_O_TRUNC      O_TRUNC
#define EU_O_APPEND     O_APPEND
#define EU_O_BINARY     O_RDONLY            // linux下不对文件区分文本还是二进制
#define EU_O_EXCL       O_EXCL
#define EU_O_TMPFILE    O_TMPFILE

#define EU_MODE        (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH) // 0664

#define EU_OPEN         ::open
#define EU_READ         ::read
#define EU_WRITE        ::write
#define EU_CLOSE        ::close
#define EU_STATF        ::stat
#define EU_SEEK         ::lseek

#define ST_MTIME        st_mtim.tv_sec
#define ST_CTIME        st_ctim.tv_sec

#else
#error "unsupport system"
#endif

namespace eular {
FileInfo::FileInfo()
{
    memset(&mFileInfo, 0, sizeof(mFileInfo));
}

FileInfo::FileInfo(const String8 &path)
{
    memset(&mFileInfo, 0, sizeof(mFileInfo));
    execGetInfo(path);
}

FileInfo::~FileInfo()
{
}

FileInfo::FileInfo(const FileInfo &other) :
    mFilePath(other.mFilePath),
    mFileInfo(other.mFileInfo)
{
}

FileInfo &FileInfo::operator=(const FileInfo &other)
{
    if (this != &other) {
        mFilePath = other.mFilePath;
        mFileInfo = other.mFileInfo;
    }

    return *this;
}

bool FileInfo::execGetInfo(const String8 &path)
{
#if defined(OS_WINDOWS)
    int32_t state = CodeConvert::UTF8ToUTF16LE(path.c_str(), mFilePath);
    if (state != STATUS(OK)) {
        return false;
    }
#else
    mFilePath = path;
#endif

    int32_t ret = EU_STATF(mFilePath.c_str(), &mFileInfo);
    if (ret) {
        LOG("get file(%s) state error: [%d:%s]", mFilePath.c_str(), errno, strerror(errno));
        return false;
    }

    return true;
}

int64_t FileInfo::getFileSize() const
{
    return mFileInfo.st_size;
}

time_t FileInfo::getModifyTime() const
{
    return mFileInfo.ST_MTIME;
}

time_t FileInfo::getCreateTime() const
{
    return mFileInfo.ST_CTIME;
}

int32_t FileInfo::getFileUid() const
{
    return static_cast<int32_t>(mFileInfo.st_uid);
}

bool FileInfo::FileExist(const String8 &path)
{
    if (path.empty()) {
        return false;
    }

#if defined(OS_WINDOWS)
    std::wstring filePath;
    int32_t status = CodeConvert::UTF8ToUTF16LE(path.toStdString(), filePath);
#else
    const String8& filePath = path;
#endif

    file_stat_t fileStat;
    int32_t ret = EU_STATF(filePath.c_str(), &fileStat);
    return ret == 0;
}

bool FileInfo::GetFileStat(const String8 &path, file_stat_t *fileStat)
{
    if (path.empty() || fileStat == nullptr) {
        return false;
    }
#if defined(OS_WINDOWS)
    std::wstring filePath;
    int32_t status = CodeConvert::UTF8ToUTF16LE(path.toStdString(), filePath);
#else
    const String8& filePath = path;
#endif
    int32_t ret = EU_STATF(filePath.c_str(), fileStat);
    return ret == 0;
}

int64_t FileInfo::GetFileSize(const String8 &path)
{
    file_stat_t fileStat;
    if (GetFileStat(path, &fileStat)) {
        return fileStat.st_size;
    }

    return -1;
}

time_t FileInfo::GetFileCreateTime(const String8 &path)
{
    file_stat_t fileStat;
    if (GetFileStat(path, &fileStat)) {
        return fileStat.ST_CTIME;
    }

    return -1;
}

time_t FileInfo::GetFileModifyTime(const String8 &path)
{
    file_stat_t fileStat;
    if (GetFileStat(path, &fileStat)) {
        return fileStat.ST_MTIME;
    }

    return -1;
}

int32_t FileInfo::GetFileUid(const String8 &path)
{
    file_stat_t fileStat;
    if (GetFileStat(path, &fileStat)) {
        return static_cast<int32_t>(fileStat.st_uid);
    }

    return -1;
}

FileOp::FileOp() :
    m_fileDesc(INVLID_FILE_DESCRIPTOR),
    m_openMode(OpenModeFlag::NotOpen)
{
    memset(&m_fileStat, 0, sizeof(file_stat_t));
}

FileOp::FileOp(const std::string& fileName) :
    m_fileDesc(INVLID_FILE_DESCRIPTOR),
    m_openMode(OpenModeFlag::NotOpen),
#if defined(OS_WINDOWS)
    m_fileNameUTF8(fileName)
#else
    m_fileName(fileName)
#endif
{
#if defined(OS_WINDOWS)
    CodeConvert::UTF8ToUTF16LE(fileName, m_fileName);
#endif
    memset(&m_fileStat, 0, sizeof(file_stat_t));
    __stat(m_fileName, &m_fileStat);
}

FileOp::FileOp(const std::string& fileName, uint32_t emMode) :
    m_fileDesc(INVLID_FILE_DESCRIPTOR),
    m_openMode(OpenModeFlag::NotOpen),
#if defined(OS_WINDOWS)
    m_fileNameUTF8(fileName)
#else
    m_fileName(fileName)
#endif
{
#if defined(OS_WINDOWS)
    CodeConvert::UTF8ToUTF16LE(fileName, m_fileName);
#endif
    memset(&m_fileStat, 0, sizeof(file_stat_t));
    open(emMode);
}

FileOp::~FileOp()
{
    close();
}

FileOp::FileOp(FileOp&& other) :
    m_fileDesc(INVLID_FILE_DESCRIPTOR),
    m_openMode(OpenModeFlag::NotOpen)
{
    std::swap(m_fileDesc, other.m_fileDesc);
    std::swap(m_fileName, other.m_fileName);

#if defined(OS_WINDOWS)
    std::swap(m_fileNameUTF8, other.m_fileNameUTF8);
#endif

    std::swap(m_openMode, other.m_openMode);
    std::swap(m_fileStat, other.m_fileStat);
}

FileOp& FileOp::operator=(FileOp&& other)
{
    if (this != &other)
    {
        std::swap(m_fileDesc, other.m_fileDesc);
        std::swap(m_fileName, other.m_fileName);

#if defined(OS_WINDOWS)
        std::swap(m_fileNameUTF8, other.m_fileNameUTF8);
#endif

        std::swap(m_openMode, other.m_openMode);
        std::swap(m_fileStat, other.m_fileStat);
    }

    return *this;
}

const std::string& FileOp::fileName() const
{
#if defined(OS_WINDOWS)
    return m_fileNameUTF8;
#else
    return m_fileName;
#endif
}

void FileOp::setFileName(const std::string& name)
{
    if (name.empty()) {
        return;
    }

#if defined(OS_WINDOWS)
    if (m_fileNameUTF8 != name) {
        m_fileNameUTF8 = name;
        CodeConvert::UTF8ToUTF16LE(name, m_fileName);
    }
#else
    if (m_fileName != name) {
        m_fileName = name;
    }
#endif
}

bool FileOp::open(uint32_t emMode)
{
    if (isOpened()) {
        return true;
    }

#if defined(OS_LINUX) || defined(OS_MACOS)
    emMode = emMode & ~(uint32_t)FileOp::Binary;
#endif

    uint32_t invalidReadWriteMode =
        (uint32_t)(FileOp::ReadOnly | FileOp::WriteOnly);
    if ((emMode & invalidReadWriteMode) == invalidReadWriteMode)
    {
        throw std::logic_error("Invalid ReadOnly | WriteOnly");
    }

    m_openMode = emMode;
    bool ret = true;
    if (emMode != OpenModeFlag::NotOpen) { // 不想打开文件, 只想获取文件信息
        int32_t flag = __flags(emMode);
        m_fileDesc = EU_OPEN(m_fileName.c_str(), flag, EU_MODE);
        if (m_fileDesc < 0)
        {
            LOG("open error: [%d:%s]", errno, strerror(errno));
        }
        ret = (m_fileDesc != INVLID_FILE_DESCRIPTOR);
    }

    ret &= __stat(m_fileName.c_str(), &m_fileStat);
    return ret;
}

bool FileOp::open(const std::string& fileName, uint32_t emMode)
{
    if (fileName.empty())
    {
        return false;
    }
    this->close();

    setFileName(fileName);
    return open(emMode);
}

int64_t FileOp::read(void* buf, int32_t size)
{
    if (nullptr == buf || 0 == size) {
        return 0;
    }

    if (!isOpened()) {
        return -EBADF;
    }

    // readSize == 0 表示读取到结尾
    // readSize < 0  表示出现异常, 可通过判断errno检测错误
    auto readSize = EU_READ(m_fileDesc, buf, size);
    if (readSize < 0) {
        LOG("read error: [%d:%s]", errno, strerror(errno));
    }
    return readSize;
}

std::string FileOp::readLine()
{
    if (!isOpened() || (m_openMode & OpenModeFlag::Binary)) {
        return std::string();
    }

    std::string line;
    char buf[2] = { 0 };  // buf[1]存放\0
    while (true) {
        int32_t n = EU_READ(m_fileDesc, buf, 1);    // 一次读取一个字符
        if (n <= 0) // 读取出错或到达结尾
        {
            break;
        }
        if (buf[0] == '\n') // windows下以文本读取时会将\r\n换成\n
        {
            break;
        }

        line.append(buf);
    }

    return line;
}

int64_t FileOp::write(const void* data, int32_t size)
{
    if (data && size > 0 && isOpened()) {
        int32_t nByteWrite = EU_WRITE(m_fileDesc, data, size);
        if (nByteWrite < 0) {
            switch (errno) {
            case EBADF:
                LOG("Bad file descriptor!(Maybe only have read permissions): [%d:%s]", errno, strerror(errno));
                break;
            case ENOSPC:
                LOG("No space left on device: [%d:%s]", errno, strerror(errno));
                break;
            case EINVAL:
                LOG("Invalid parameter: buffer was NULL: [%d:%s]", errno, strerror(errno));
                break;
            default:
                // An unrelated error occurred
                LOG("Unexpected error: [%d:%s]", errno, strerror(errno));
            }
        }
        return nByteWrite;
    }

    return -1;
}

int64_t FileOp::write(const std::string& str)
{
    return write(str.c_str(), str.length());
}

int64_t FileOp::write(const std::stringstream& ss)
{
    return write(ss.str());
}

int64_t FileOp::pos() const
{
    // int32_t currentPos = EU_SEEK(m_fileDesc, 0, SEEK_CUR);
    // int32_t postion = EU_SEEK(m_fileDesc, 0, SEEK_END);
    // EU_SEEK(m_fileDesc, currentPos, SEEK_SET);
    return EU_SEEK(m_fileDesc, 0, SEEK_CUR);
}

bool FileOp::seek(int64_t offset, int32_t whence)
{
    if (!isOpened()) {
        return false;
    }

    switch (whence) {
    case SEEK_SET:
    case SEEK_CUR:
    case SEEK_END:
        break;
    default:
        return false;
    }

    int64_t ret = EU_SEEK(m_fileDesc, offset, whence);
    return static_cast<bool>(ret != -1);
}

// NOTE 无法直接和fileSize()进行判断，有可能因为是新建文件，导致通过stat获取的文件大小为0
bool FileOp::eof() const
{
    int32_t currentPos = pos();
    EU_SEEK(m_fileDesc, 0, SEEK_END);
    int32_t endPos = pos();
    if (currentPos < endPos) {
        EU_SEEK(m_fileDesc, currentPos, SEEK_SET);
        return false;
    }

    return true;
}

bool FileOp::flush()
{
#if defined(OS_WINDOWS)
    _flushall();
    return true;
#else
    return (-1 != fsync(m_fileDesc));
#endif
}

int64_t FileOp::fileSize() const
{
    return static_cast<int64_t>(m_fileStat.st_size);
}

void FileOp::close()
{
    if (m_fileDesc != INVLID_FILE_DESCRIPTOR) {
        EU_CLOSE(m_fileDesc);
        m_fileDesc = INVLID_FILE_DESCRIPTOR;
    }
}

bool FileOp::isOpened() const
{
    return (INVLID_FILE_DESCRIPTOR != m_fileDesc);
}

#if defined(OS_WINDOWS)
bool FileOp::__stat(const std::wstring& file, file_stat_t* info)
#else
bool FileOp::__stat(const std::string& file, file_stat_t* info)
#endif
{
    assert(info);
    int32_t nErr = EU_STATF(m_fileName.c_str(), info);
    return static_cast<bool>(nErr == 0);
}

#define OPEN_MODE_UNFOLD(XXX)                       \
    XXX(OpenModeFlag::ReadOnly,     EU_O_RDONLY)    \
    XXX(OpenModeFlag::WriteOnly,    EU_O_WRONLY)    \
    XXX(OpenModeFlag::ReadWrite,    EU_O_RDWR)      \
    XXX(OpenModeFlag::Append,       EU_O_APPEND)    \
    XXX(OpenModeFlag::Truncate,     EU_O_TRUNC)     \
    XXX(OpenModeFlag::Binary,       EU_O_BINARY)    \
    XXX(OpenModeFlag::Temporary,    EU_O_TMPFILE)   \
    XXX(OpenModeFlag::Create,       EU_O_CREAT)     \

int32_t FileOp::__flags(uint32_t emMode)
{
    int32_t flags = 0;

#define XXX(mode, flag)     \
    if (emMode & mode)      \
    {                       \
        flags |= flag;      \
    }                       \

    OPEN_MODE_UNFOLD(XXX);
#undef XXX

    // O_RDWR 无法与 O_WRONLY, O_RDONLY共存
    if (flags & EU_O_RDWR)
    {
        flags &= ~(EU_O_WRONLY | EU_O_RDONLY);
    }

    return flags;
}

} // namespace eular
