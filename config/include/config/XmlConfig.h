/*************************************************************************
    > File Name: XmlConfig.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 12 Jan 2023 10:18:27 AM CST
 ************************************************************************/

#ifndef __CONFIG_XML_CONFIG_H__
#define __CONFIG_XML_CONFIG_H__

#include <config/type_cast.h>
#include <string>
#include <map>

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
        const std::string &val = lookup(key);
        if (val.empty()) {
            return defaultVal;
        }

        T ret = defaultVal;
        try {
            ret = boost::lexical_cast<T>(val.c_str());
        } catch (...) {
            printf("XmlConfig::lookup<%s>(key = %s) conversion failed\n", typeid(T).name(), key.c_str());
        }

        return ret;
    }

    void foreach();

protected:
    void loadXml(std::string prefix, void *ele);
    std::string lookup(const std::string &key);

private:
    void *m_mutex;
    std::map<std::string, std::string> m_xmlMap;
};

} // namespace eular

#endif // __CONFIG_XML_CONFIG_H__
