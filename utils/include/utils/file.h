#ifndef __FILE_OPERATOR__
#define __FILE_OPERATOR__

#include <string>
#include <sstream>
#include <memory>

#include <sys/stat.h>

#include <utils/sysdef.h>
#include <utils/string8.h>

#ifdef OS_WINDOWS
#define file_stat_t    struct _stat
#elif defined(OS_UNIX)
#define file_stat_t    struct stat
#endif

namespace eular {
class UTILS_API FileInfo
{
    friend class File;

public:
    typedef std::unique_ptr<FileInfo>   Ptr;
    typedef std::shared_ptr<FileInfo>   SP;
    typedef std::weak_ptr<FileInfo>     WP;

    FileInfo();
    FileInfo(const String8 &path);
    ~FileInfo();

    FileInfo(const FileInfo &other);
    FileInfo &operator=(const FileInfo &other);

    bool    execGetInfo(const String8 &path);
    int64_t getFileSize() const;
    time_t  getModifyTime() const;
    time_t  getCreateTime() const;
    int32_t getFileUid() const;
    const file_stat_t &getFileStat() const { return mFileInfo; }

    static bool     FileExist(const String8 &path);
    static bool     GetFileStat(const String8 &path, file_stat_t *fileStat);
    static int64_t  GetFileSize(const String8 &path);
    static time_t   GetFileCreateTime(const String8 &path);
    static time_t   GetFileModifyTime(const String8 &path);
    static int32_t  GetFileUid(const String8 &path);

private:
    String8     mFilePath;
    file_stat_t mFileInfo;
};

class UTILS_API FileOp
{
    FileOp(const FileOp& other) = delete;
    FileOp& operator=(const FileOp& other) = delete;
public:
    enum OpenModeFlag {
        NotOpen		= 0x0000,
        ReadOnly	= 0x0001,
        WriteOnly	= 0x0002,
        ReadWrite	= 0x0004,
        Create		= 0x0008,
        Append		= 0x0010,
        Truncate	= 0x0020,
        Binary		= 0x0040,
        Temporary	= 0x0080,
    };

    FileOp();
    FileOp(const std::string& fileName);
    FileOp(const std::string& fileName, uint32_t emMode);
    ~FileOp();

    FileOp(FileOp&& other);
    FileOp& operator=(FileOp&& other);

    const std::string& fileName() const;
    void setFileName(const std::string& name);

    bool open(uint32_t emMode = ReadOnly);

    /**
     * @brief 由 filename 指定的以 emMode 方式打开文件。
     *        当文件名有修改时，会覆盖之前的文件名并关闭之前文件，再以emMode重新打开文件
     * @param fileName 文件名(包含路径信息)
     * @param emMode 打开方式
     * @return
    */
    bool open(const std::string& fileName, uint32_t emMode = ReadOnly);

    /**
     * @brief 读取最大size字节到buf
     * @param buf 缓冲区
     * @param size 大小
     * @return 成功返回实际读取的字节数，失败返回-1
     */
    int64_t read(void* buf, int32_t size);

    /**
     * @brief 读取一行(只针对文本), 不包含最后换行符
     * @return 成功返回返回读取的内容，失败返回空
     */
    std::string readLine();

    int64_t write(const void* data, int32_t size);

    int64_t write(const std::string& str);

    int64_t write(const std::stringstream& ss);

    /**
     * @brief 获取当前指针位置
     * 
     * @return 返回相较于起点的偏移值
     */
    int64_t pos() const;

    /**
     * @brief 将文件指针偏移到指定位置
     *
     * @param offset 偏移量
     * @param whence 偏移起点(可选SEEK_SET, SEEK_CUR, SEEK_END)
     * @return 成功返回true, 失败返回false
     */
    bool seek(int64_t offset, int32_t whence);
    bool eof() const;
    bool flush();

    int64_t fileSize() const;

    /**
     * @brief 关闭文件句柄
     */
    void close();

    /**
     * @brief 判断文件是否打开
     * @return 已打开返回true，未打开返回false
    */
    bool isOpened() const;

    int32_t getFd() const { return m_fileDesc; }

    file_stat_t getStat() const { return m_fileStat; }

private:
    /**
     * @brief 获取file指定文件的信息，放到info中
     *
     * @param file utf8格式文件名(包含路径信息)
     * @param info 结构体地址, 不允许为空
     * @return 成功返回true, 失败返回false
     */
#if defined(OS_WINDOWS)
    bool __stat(const std::wstring& file, file_stat_t* info);
#elif defined(OS_UNIX)
    bool __stat(const std::string& file, file_stat_t* info);
#endif
    int32_t __flags(uint32_t emMode);

private:
    int32_t         m_fileDesc;
    uint32_t        m_openMode;
#if defined(OS_WINDOWS)
    std::wstring    m_fileName;
#elif defined(OS_UNIX)
    std::string     m_fileName;
#endif
    file_stat_t        m_fileStat;

#if defined(OS_WINDOWS)
    std::string     m_fileNameUTF8;
#endif
};

} // namespace eular
#endif // !__FILE_OPERATOR__
