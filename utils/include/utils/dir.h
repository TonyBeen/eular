/*************************************************************************
    > File Name: dir.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年11月21日 星期四 17时15分38秒
 ************************************************************************/

#ifndef __UTILS_DIR_H__
#define __UTILS_DIR_H__

#include <string>

#include <utils/sysdef.h>

namespace eular {
namespace dir {

/**
 * @brief 判断路径或文件是否存在
 *
 * @param path 路径
 * @return true 
 * @return false 
 */
UTILS_API bool exists(const std::string &path);

/**
 * @brief 根据给定的带有.或..路径返回绝对路径
 * 如
 * ~/path/../A/./B/ => /home/user/A/B/
 * /home/user/../other/./../third => /home/third/
 * 
 * 由于没有给定基路径, 故无法计算相对路径, 如 ../A/B/./C
 * 
 * @param path 路径
 * @param absPath 输出绝对路径
 * @return true 
 * @return false 
 */
UTILS_API bool absolute(const std::string &path, std::string &absPath);

/**
 * @brief 创建路径, 类似 mkdir -p /path/to/non-existent
 * 
 * @param path 路径
 * @return true 
 * @return false 
 */
UTILS_API bool mkdir(const std::string &path);

} // namespace dir
} // namespace eular

#endif // __UTILS_DIR_H__
