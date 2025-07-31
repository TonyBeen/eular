/*************************************************************************
    > File Name: IniConfig.cpp
    > Author: hsz
    > Brief:
    > Created Time: Tue 27 Dec 2022 04:00:06 PM CST
 ************************************************************************/

#include "IniConfig.h"
#include <string.h>
#include <exception>
#include <vector>
#include <sstream>

#define GLOBAL_NODE_NAME    ""
#define SEPARATOR           "."
#define WRAP                "\n"

namespace eular {

/**
 * @brief 去除字符串str左右两次多余的字符c
 * @param str 
 * @param c 
 */
bool trim(std::string &str, const char *c = " \t\r\n")
{
    size_t begin = str.find_first_not_of(c);
    if (begin == std::string::npos) {
        return false;
    }

    size_t end = str.find_last_not_of(c);
    if (end == std::string::npos) {
        return false;
    }

    if (begin == end) {
        return false;
    }

    std::string temp(str.c_str() + begin, end - begin + 1);
    std::swap(temp, str);
    return true;
}

IniConfig::IniConfig()
{

}

IniConfig::~IniConfig()
{

}

bool IniConfig::parser(const std::string &configPath)
{
    static const std::string escapeString = "abcdefghijklmnopqrstuvwxyz_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    reset();
    mConfigFilePath = configPath;
    FILE *filePtr = fopen(configPath.c_str(), "r");
    if (filePtr == nullptr) {
        perror("fopen error");
        return false;
    }

    std::string nodeName = GLOBAL_NODE_NAME;

    char buf[1024];
    char *pos = nullptr;
    size_t index = 0;
    while (feof(filePtr) == false) {
        pos = fgets(buf, sizeof(buf), filePtr);
        if (pos == nullptr) {
            break;
        }

        std::string line(pos);
        // 处理注释部分
        if (pos[0] == ';') {
            continue;
        }
        if ((index = line.find(';')) != std::string::npos) {
            line[index] = '\0';
        }

        if (trim(line) == false) {
            continue;
        }
        // printf("line = \"%s\"\n", line.c_str());

        pos = &line[0];
        char *left = strchr(pos, '[');
        char *right = strchr(pos, ']');
        if (left && right && left < right) {
            nodeName = std::string(left + 1, right - left - 1);
            trim(nodeName);
            // printf("node name = \"%s\"\n", nodeName.c_str());
            if (nodeName.find_first_not_of(escapeString) != std::string::npos) {
                throw std::logic_error("node name contains invalid charactor!");
            }
            continue;
        }

        char *equalChar = strchr(pos, '='); 
        if (equalChar == nullptr) {
            throw std::logic_error("invalid key value pair");
        }

        std::string key(pos, equalChar - 1);
        std::string val(equalChar + 1);
        trim(key);
        trim(val);
        // printf("key - value: [%s = %s]\n", key.c_str(), val.c_str());
        if (key.find_first_not_of(escapeString) != std::string::npos) {
            throw std::logic_error("key name contains invalid charactor!");
        }

        if (nodeName == GLOBAL_NODE_NAME) {
            mConfigMap[key] = val;
        } else {
            mConfigMap[nodeName + "." + key] = val;
        }
        mSourceMap[nodeName].insert(std::make_pair(key, val));
    }

    return true;
}

std::string IniConfig::value(const std::string &key)
{
    auto it = mConfigMap.find(key);
    if (it == mConfigMap.end()) {
        return std::string();
    }

    return it->second;
}

std::string &IniConfig::operator[](const std::string &key)
{
    std::string nodeName, keyName;
    size_t pos = key.find('.');
    if (pos == std::string::npos) {
        nodeName = GLOBAL_NODE_NAME;
        keyName = key;
    } else {
        nodeName = std::string(key.c_str(), pos - 1);
        keyName = std::string(key.c_str() + pos + 1);
    }

    auto mapIt = mSourceMap.find(nodeName);
    if (mapIt == mSourceMap.end()) {
        std::map<std::string, std::string> m;
        m[keyName] = std::string();
        mSourceMap[nodeName] = m;
        return mSourceMap[nodeName].find(keyName)->second;
    }

    return mapIt->second[keyName];
}

bool IniConfig::del(const std::string &key)
{
    auto it = mConfigMap.find(key);
    if (it == mConfigMap.end()) {
        return true;
    }

    mConfigMap.erase(it);
    std::string nodeName, keyName;
    size_t pos = key.find('.');
    if (pos == std::string::npos) {
        nodeName = GLOBAL_NODE_NAME;
        keyName = key;
    } else {
        nodeName = std::string(key.c_str(), pos - 1);
        keyName = std::string(key.c_str() + pos + 1);
    }
    
    auto iter = mSourceMap.find(nodeName);
    if (iter == mSourceMap.end()) {
        return true;
    }

    iter->second.erase(keyName);
    return true;
}

bool IniConfig::keep(std::string file)
{
    if (file.length() == 0) {
        file = mConfigFilePath;
    }
    FILE *fp = fopen(file.c_str(), "w");
    if (fp == nullptr) {
        perror("fopen error");
        return false;
    }

    std::stringstream nodeStream;
    std::stringstream globalStream;
    char buf[512] = {0};
    for (auto it = mSourceMap.begin(); it != mSourceMap.end(); ++it) {
        if (it->first == GLOBAL_NODE_NAME) {
            const auto &globalMap = it->second;
            for (auto git = globalMap.begin(); git != globalMap.end(); ++git) {
                snprintf(buf, sizeof(buf), "%s = %s" WRAP, git->first.c_str(), git->second.c_str());
                globalStream << buf;
            }
            globalStream << WRAP;
        } else {
            snprintf(buf, sizeof(buf), "[%s]" WRAP, it->first.c_str());
            nodeStream << buf;
            const auto &nodeMap = it->second;
            for (auto nit = nodeMap.begin(); nit != nodeMap.end(); ++nit) {
                snprintf(buf, sizeof(buf), "%s = %s" WRAP, nit->first.c_str(), nit->second.c_str());
                nodeStream << buf;
            }

            nodeStream << WRAP;
        }
    }
    
    std::string global = globalStream.str();
    std::string node = nodeStream.str();
    if (global.length() > 0) {
        fwrite(global.c_str(), sizeof(char), global.length(), fp);
    }
    if (node.length() > 0) {
        fwrite(node.c_str(), sizeof(char), node.length() - strlen(WRAP), fp); // 去除最后的换行符
    }

    fclose(fp);
}

void IniConfig::reset()
{
    mConfigFilePath.clear();
    mConfigMap.clear();
    mSourceMap.clear();
}

} // namespace eular
