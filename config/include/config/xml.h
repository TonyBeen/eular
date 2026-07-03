/*************************************************************************
    > File Name: xml.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 12 Jan 2023 10:18:27 AM CST
 ************************************************************************/

#ifndef __CONFIG_XML_CONFIG_H__
#define __CONFIG_XML_CONFIG_H__

#include <stdio.h>
#include <cctype>
#include <typeinfo>
#include <string>
#include <map>

#include <c4/charconv.hpp>
#include <c4/std/string.hpp>

#include <config/exports.h>

namespace eular {

class CONFIG_API XmlConfig
{
public:
    XmlConfig();
    ~XmlConfig();

    bool loadFile(const std::string &xmlFile);
    bool parse(const std::string &xmlText);

    template<typename T>
    T lookup(const std::string &key, const T &defaultVal = T())
    {
        std::string val = lookup(key);
        if (val.empty()) {
            return defaultVal;
        }

        T ret = defaultVal;
        if (!ParseValue(val, ret)) {
            return defaultVal;
        }

        return ret;
    }

    void foreach();

protected:
    void loadXml(std::string prefix, void *ele);
    std::string lookup(const std::string &key);

private:
    static std::string TrimScalar(const std::string &value)
    {
        size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            return std::string();
        }

        size_t end = value.find_last_not_of(" \t\r\n");
        return std::string(value.c_str() + begin, end - begin + 1);
    }

    template<typename T>
    static bool ParseValue(const std::string &text, T &value)
    {
        std::string trimmed = TrimScalar(text);
        if (trimmed.empty()) {
            return false;
        }

        return c4::from_chars(c4::to_csubstr(trimmed), &value);
    }

    static bool ParseValue(const std::string &text, std::string &value)
    {
        value = text;
        return true;
    }

    static bool ParseValue(const std::string &text, bool &value)
    {
        std::string trimmed = TrimScalar(text);
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

    static bool ParseValue(const std::string &, const char *&)
    {
        return false;
    }

private:
    std::map<std::string, std::string> m_xmlMap;
};

} // namespace eular

#endif // __CONFIG_XML_CONFIG_H__
