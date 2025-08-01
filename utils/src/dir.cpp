/*************************************************************************
    > File Name: dir.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年11月21日 星期四 17时15分50秒
 ************************************************************************/

#include "utils/dir.h"

#include <sstream>
#include <deque>
#include <vector>
#include <algorithm>

#include <sys/stat.h>
#include <sys/types.h>

#include "utils/sysdef.h"
#include "utils/code_convert.h"
#include "utils/debug.h"

#if defined(OS_LINUX)
#include <pwd.h>
#include <unistd.h>
#elif defined(OS_APPLE)
#else
#include <direct.h>
#include <io.h>
#endif

namespace eular {
namespace dir {
std::string AdjustPath(std::string path, char oldCh = '\\', char newCh = '/')
{
	if (path.empty()) {
		return path;
	}

	std::replace(path.begin(), path.end(), oldCh, newCh);

	// 去除路径中的多余分隔符
    auto new_end = std::unique(path.begin(), path.end(), [](char a, char b) {
		return a == b && a == '/';
	});
	path.erase(new_end, path.end());
	return path;
}

bool exists(const std::string &path)
{
#if defined(OS_LINUX) || defined(OS_APPLE)
    if (path.empty()) {
        return false;
    }

    struct stat lst;
    int32_t ret = ::lstat(path.c_str(), &lst);
    return ret == 0;
#else
    std::wstring wpath;
    CodeConvert::UTF8ToUTF16LE(path, wpath);

    struct _stat lst;
    int32_t ret = ::_wstat(wpath.c_str(), &lst);
    return ret == 0;
#endif
}

bool __mkdir(const char *path)
{
#if defined(OS_LINUX) || defined(OS_APPLE)
    if (access(path, F_OK) == 0) {
        return true;
    }

    return 0 == ::mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
#else
    std::wstring wpath;
    CodeConvert::UTF8ToUTF16LE(path, wpath);

    if (::_waccess(wpath.c_str(), 0) == 0) {
        return true;
    }

    return 0 == ::_wmkdir(wpath.c_str());
#endif
}

std::vector<std::string> _splitPath(const std::string& path) {
    std::vector<std::string> result;
    std::stringstream ss(path.c_str());

    std::string segment;
    while (std::getline(ss, segment, DIR_SEPARATOR)) {
        if (!segment.empty()) {
            result.push_back(segment);
        }
    }

    return result;
}

bool absolute(const std::string &path, std::string &absPath)
{
    if (path.empty()) {
        return false;
    }

    std::deque<std::string> pathDeque;
    pathDeque.push_back("/");

    std::string realPath = AdjustPath(path);
#if defined(OS_LINUX) || defined(OS_APPLE)
    if (path.front() == '~') {
        struct passwd result;
        struct passwd *pw = nullptr;
        char buffer[1204] = {0};
        int32_t code = getpwuid_r(getuid(), &result, buffer, sizeof(buffer), &pw);
        if (code != 0 || pw == nullptr) {
            LOG("getpwuid error: [%d,%s]", errno, strerror(errno));
            return false;
        }

        realPath = pw->pw_dir;
        realPath.append(path.c_str() + 1);
    }
#else
	char driveLetter[8] = { 0 };
	if (realPath.length() > 2 && realPath[1] == ':') {
		driveLetter[0] = realPath[0];
		driveLetter[1] = ':';
		driveLetter[2] = '\0';
		realPath.erase(0, 2);
	}
#endif
    if (realPath.front() != '/') {
        return false;
    }

    if (realPath.find('.') == std::string::npos && realPath.find("..") == std::string::npos) { // 不存在相对路径符号
        absPath = realPath;
        return true;
    }

    std::vector<std::string> segments = _splitPath(realPath);
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i] == ".") {
            // do nothing
        } else if (segments[i] == "..") {
            if (pathDeque.size() > 1) { // 保留 跟路径 /
                pathDeque.pop_back();
            }
        } else {
            pathDeque.push_back(segments.at(i));
        }
    }

    absPath.reserve(path.length());
    while (!pathDeque.empty()) {
        absPath += pathDeque.front();
        if (absPath.back() != DIR_SEPARATOR) {
            absPath.push_back(DIR_SEPARATOR);
        }
        pathDeque.pop_front();
    }

#if defined(OS_WINDOWS)
	// 在 Windows 上添加驱动器字母
	absPath.insert(absPath.begin(), driveLetter, driveLetter + 2);
#endif
    return true;
}

bool mkdir(const std::string &path)
{
    if (exists(path)) {
        return true;
    }

    std::string realPath;
    if (!absolute(path, realPath)) {
        return false;
    }

    char* filePath = strdup(realPath.c_str());
    char* ptr = strchr(filePath + 1, DIR_SEPARATOR);
    do {
        for (; ptr; *ptr = DIR_SEPARATOR, ptr = strchr(ptr + 1, DIR_SEPARATOR)) {
            *ptr = '\0';
            if (__mkdir(filePath) != 0) {
                break;
            }
        }

        if (ptr != nullptr) {
            break;
        } else if (!__mkdir(filePath)) {
            break;
        }

        free(filePath);
        return true;
    } while(0);

    free(filePath);
    return false;
}

}
// namespace dir
} // namespace eular
