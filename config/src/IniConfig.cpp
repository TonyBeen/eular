/*************************************************************************
    > File Name: IniConfig.cpp
    > Author: hsz
    > Brief:
    > Created Time: Tue 27 Dec 2022 04:00:06 PM CST
 ************************************************************************/

#include "config/ini.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <sstream>
#include <memory>

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

    std::string temp(str.c_str() + begin, end - begin + 1);
    std::swap(temp, str);
    return true;
}

static void splitKey(const std::string &fullKey, std::string &nodeName, std::string &keyName)
{
    size_t pos = fullKey.find('.');
    if (pos == std::string::npos) {
        nodeName = GLOBAL_NODE_NAME;
        keyName = fullKey;
        return;
    }

    nodeName = fullKey.substr(0, pos);
    keyName = fullKey.substr(pos + 1);
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
    FILE *rawFile = fopen(configPath.c_str(), "r");
    if (rawFile == nullptr) {
        perror("fopen error");
        return false;
    }

    std::unique_ptr<FILE, int(*)(FILE*)> filePtr(rawFile, fclose);

    std::string nodeName = GLOBAL_NODE_NAME;

    char buf[1024];
    char *pos = nullptr;
    size_t index = 0;
    while ((pos = fgets(buf, sizeof(buf), filePtr.get())) != nullptr) {

        std::string line(pos);
        // 处理注释部分
        if (pos[0] == ';') {
            continue;
        }
        if ((index = line.find(';')) != std::string::npos) {
            line.erase(index);
        }

        if (trim(line) == false) {
            continue;
        }

        pos = &line[0];
        char *left = strchr(pos, '[');
        char *right = strchr(pos, ']');
        if (left && right && left < right) {
            nodeName = std::string(left + 1, right - left - 1);
            trim(nodeName);
            if (!nodeName.empty() && nodeName.find_first_not_of(escapeString) != std::string::npos) {
                return false;
            }
            continue;
        }

        char *equalChar = strchr(pos, '='); 
        if (equalChar == nullptr) {
            return false;
        }

        std::string key(pos, static_cast<size_t>(equalChar - pos));
        std::string val(equalChar + 1);
        trim(key);
        trim(val);
        if (key.empty() || key.find_first_not_of(escapeString) != std::string::npos) {
            return false;
        }

        std::string fullKey = nodeName == GLOBAL_NODE_NAME ? key : (nodeName + SEPARATOR + key);
        mConfigMap[fullKey] = val;
        mSourceMap[nodeName][key] = val;
    }

    return true;
}

std::string IniConfig::value(const std::string &key) const
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
    splitKey(key, nodeName, keyName);

    std::string fullKey = nodeName == GLOBAL_NODE_NAME ? keyName : (nodeName + SEPARATOR + keyName);
    std::string &valueRef = mConfigMap[fullKey];
    mSourceMap[nodeName][keyName] = valueRef;
    return valueRef;
}

bool IniConfig::del(const std::string &key)
{
    std::string nodeName, keyName;
    splitKey(key, nodeName, keyName);

    std::string fullKey = nodeName == GLOBAL_NODE_NAME ? keyName : (nodeName + SEPARATOR + keyName);
    mConfigMap.erase(fullKey);
    
    auto iter = mSourceMap.find(nodeName);
    if (iter != mSourceMap.end()) {
        iter->second.erase(keyName);
        if (iter->second.empty()) {
            mSourceMap.erase(iter);
        }
    }

    return true;
}

bool IniConfig::keep(std::string file)
{
    if (file.length() == 0) {
        file = mConfigFilePath;
    }
    FILE *rawFile = fopen(file.c_str(), "w");
    if (rawFile == nullptr) {
        perror("fopen error");
        return false;
    }

    std::unique_ptr<FILE, int(*)(FILE*)> fp(rawFile, fclose);

    mSourceMap.clear();
    for (auto it = mConfigMap.begin(); it != mConfigMap.end(); ++it) {
        std::string nodeName;
        std::string keyName;
        splitKey(it->first, nodeName, keyName);
        mSourceMap[nodeName][keyName] = it->second;
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
    bool writeOk = true;
    if (global.length() > 0) {
        writeOk = fwrite(global.c_str(), sizeof(char), global.length(), fp.get()) == global.length();
    }
    if (node.length() > 0) {
        size_t writeLen = node.length() > strlen(WRAP) ? (node.length() - strlen(WRAP)) : 0;
        if (writeLen > 0) {
            writeOk = writeOk && (fwrite(node.c_str(), sizeof(char), writeLen, fp.get()) == writeLen); // 去除最后的换行符
        }
    }

    return writeOk && ferror(fp.get()) == 0;
}

void IniConfig::reset()
{
    mConfigFilePath.clear();
    mConfigMap.clear();
    mSourceMap.clear();
}

} // namespace eular
