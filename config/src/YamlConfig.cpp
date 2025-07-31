/*************************************************************************
    > File Name: YamlConfig.cpp
    > Author: hsz
    > Brief:
    > Created Time: Thu 29 Dec 2022 01:54:09 PM CST
 ************************************************************************/

#include "YamlConfig.h"
#include <assert.h>
#include <exception>
#include <regex>
#include <fstream>

namespace eular {

static inline RWMutex *toMutex(void *mtx)
{
    return static_cast<RWMutex *>(mtx);
}

YamlReader::YamlReader() :
    isValid(false),
    mMutex(new RWMutex)
{
}

YamlReader::YamlReader(const std::string &path) :
    isValid(false),
    mMutex(new RWMutex)
{
    loadYaml(path);
}

YamlReader::~YamlReader()
{
    isValid = false;
    toMutex(mMutex)->wlock();
    mYamlConfigMap.clear();
    toMutex(mMutex)->unlock();

    delete (RWMutex *)mMutex;
}

void YamlReader::loadYaml(const std::string &path)
{
    toMutex(mMutex)->wlock();
    mYamlPath = path;
    mYamlConfigMap.clear();
    try {
        mYamlRoot = YAML::LoadFile(mYamlPath);
        if (mYamlRoot.IsNull()) {
            goto _unlock;
        }
        loadYaml("", mYamlRoot);
        isValid = true;
    } catch (const std::exception &e) {
        printf("%s() error. %s\n", __func__, e.what());
    }

_unlock:
    toMutex(mMutex)->unlock();
}

YamlValue YamlReader::at(const std::string &key)
{
    YamlValue node;
    toMutex(mMutex)->rlock();
    auto it = mYamlConfigMap.find(key);
    if (it != mYamlConfigMap.end()) {
        node = it->second;
    }
    toMutex(mMutex)->unlock();

    return node;
}

YamlValue YamlReader::root()
{
    toMutex(mMutex)->rlock();
    YamlValue node = mYamlConfigMap[""];
    toMutex(mMutex)->unlock();

    return node;
}

void YamlReader::foreach(std::string &output, bool outValue)
{
    toMutex(mMutex)->rlock();
    std::stringstream strstream;
    for (auto it = mYamlConfigMap.begin(); it != mYamlConfigMap.end(); ++it) {
        // std::cout << it->first << ": " << it->second << std::endl;
        strstream << it->first.c_str();
        if (outValue) {
            strstream << " -- " << it->second;
        }
        strstream << std::endl;
    }

    output = strstream.str();
    toMutex(mMutex)->unlock();
}

void YamlReader::loadYaml(const std::string &prefix, const YamlValue &node)
{
    static const std::regex regex("^[A-Za-z0-9\\s\\._]*$");
    if (!std::regex_match(prefix, regex)) {
        printf("Config invalid prefix: %s\n", prefix.c_str());
        return;
    }

    // static const std::string ext = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._1234567890";
    // if (prefix.find_first_not_of(ext) != std::string::npos) {
    //     printf("Config invalid prefix: %s\n", prefix.c_str());
    //     return;
    // }

    mYamlConfigMap.insert(std::make_pair(prefix, node));
    if (node.IsMap()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            loadYaml(prefix.empty() ? it->first.Scalar() : prefix + "." + it->first.Scalar(), it->second);
        }
    }
    // else if (node.IsSequence()) { // 处理数组+map组合
    //     for (int i = 0; i < node.size(); ++i) {
    //         const YAML::Node &temp = node[i];
    //         if (temp.IsMap()) {
    //             for (auto it = temp.begin(); it != temp.end(); ++it) {
    //                 loadYaml(prefix.empty() ? it->first.Scalar() : prefix + "." + it->first.Scalar(), it->second);
    //             }
    //         }
    //     }
    // }
}

void YamlReader::rlock()
{
    toMutex(mMutex)->rlock();
}

void YamlReader::runlock()
{
    toMutex(mMutex)->unlock();
}

} // namespace eular
