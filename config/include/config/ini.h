/*************************************************************************
    > File Name: ini.h
    > Author: hsz
    > Brief: ini文件解析
    > Created Time: Tue 27 Dec 2022 04:00:03 PM CST
 ************************************************************************/

#ifndef __INI_CONFIG_H__
#define __INI_CONFIG_H__

#include <cctype>
#include <string>
#include <map>
#include <vector>

#include <c4/charconv.hpp>
#include <c4/std/string.hpp>

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

    std::string value(const std::string &key) const;

    template<typename T>
    T lookup(const std::string &key, const T &defaultVal = T()) const
    {
        std::string text = value(key);
        if (text.empty()) {
            return defaultVal;
        }

        T converted = defaultVal;
        if (!parseValue(text, converted)) {
            return defaultVal;
        }

        return converted;
    }

    std::string &operator[](const std::string &key);
    bool del(const std::string &key);
    bool keep(std::string file = "");

protected:
    void reset();

private:
    static std::string trimScalar(const std::string &value)
    {
        size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            return std::string();
        }

        size_t end = value.find_last_not_of(" \t\r\n");
        return std::string(value.c_str() + begin, end - begin + 1);
    }

    template<typename T>
    static bool parseValue(const std::string &text, T &value)
    {
        std::string trimmed = trimScalar(text);
        if (trimmed.empty()) {
            return false;
        }

        return c4::from_chars(c4::to_csubstr(trimmed), &value);
    }

    static bool parseValue(const std::string &text, std::string &value)
    {
        value = text;
        return true;
    }

    static bool parseValue(const std::string &text, bool &value)
    {
        std::string trimmed = trimScalar(text);
        if (trimmed.empty()) {
            return false;
        }

        std::string lower(trimmed);
        for (size_t i = 0; i < lower.size(); ++i) {
            lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[i])));
        }

        if (lower == "true" || lower == "yes") {
            value = true;
            return true;
        }
        if (lower == "false" || lower == "no") {
            value = false;
            return true;
        }

        return c4::from_chars(c4::to_csubstr(trimmed), &value);
    }

    static bool parseValue(const std::string &, const char *&)
    {
        return false;
    }

    std::string mConfigFilePath;
    std::map<std::string, std::string> mConfigMap;
    std::map<std::string, std::map<std::string, std::string>> mSourceMap;
};

} // namespace eular

#endif