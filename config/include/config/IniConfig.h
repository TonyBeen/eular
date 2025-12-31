/*************************************************************************
    > File Name: IniConfig.h
    > Author: hsz
    > Brief: ini文件解析
    > Created Time: Tue 27 Dec 2022 04:00:03 PM CST
 ************************************************************************/

#ifndef __INI_CONFIG_H__
#define __INI_CONFIG_H__

#include <string>
#include <map>
#include <vector>

namespace eular {

/**
 * @brief 解析ini文件类
 * ini由节, 键, 值组成
 * 节键值都是字符串形式, 注释由;表示, 且只能注释单行. 节禁止重复, 但可以不存在, 键可以重复
 * 
 * 故解析后的键为"节.键". 如
 * 
 * [node]
 * host = 127.0.0.1
 * port = 2000
 * 
 * [node2]
 * host = 127.0.0.1
 * port = 2000
 * 
 * 解析后想要获取host的值可以使用node.host来获取,也可以用node2.host获取第二个host的值
 * 
 * 一行最多1024个字符, UTF-8编码
 */

class IniConfig
{
public:
    IniConfig();
    ~IniConfig();

    bool parser(const std::string &configPath);

    std::string value(const std::string &key);
    std::string &operator[](const std::string &key);
    bool del(const std::string &key);
    bool keep(std::string file = "");

protected:
    void reset();

private:
    std::string mConfigFilePath;
    std::map<std::string, std::string> mConfigMap;
    std::map<std::string, std::map<std::string, std::string>> mSourceMap;
};

} // namespace eular

#endif