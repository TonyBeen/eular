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

namespace eular {

class XmlConfig
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
        if (!parseValue(val, ret)) {
            printf("XmlConfig::lookup<%s>(key = %s) conversion failed\n", typeid(T).name(), key.c_str());
            return defaultVal;
        }

        return ret;
    }

    void foreach();

protected:
    void loadXml(std::string prefix, void *ele);
    std::string lookup(const std::string &key);

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

    void *m_mutex;
    std::map<std::string, std::string> m_xmlMap;
};

} // namespace eular

#endif // __CONFIG_XML_CONFIG_H__
