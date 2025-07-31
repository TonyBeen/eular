/*************************************************************************
    > File Name: YamlConfig.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 29 Dec 2022 01:54:06 PM CST
 ************************************************************************/

#ifndef __CONFIG_YAML_CONFIG_H__
#define __CONFIG_YAML_CONFIG_H__

#include <string>
#include <map>

#include <yaml-cpp/yaml.h>

#include <utils/singleton.h>

/**
 * @brief Yaml文件解析类
 * 只能解析map类型的yaml文件, 如果其中存在数组可以通过at函数获取源数据, 自行解析。
 * 如下一段配置文件
 * spec:
 *   selector:
 *     app: nginx
 *   volum:
 *     containers:
 *       - image: nginx:1.14
 *         imagePullPolicy: IfNotPresent
 *       - image: websocket:2.10
 *         imagePullPolicy: IfNotPresent
 * log:
 *   level: info
 *   target: stdout
 *   sync: true
 *   int: 10
 *   double: 3.1415
 *
 * 想要获取containers的数组, 可以通过 YamlReader->at("spec.volum.containers"); 获取
 * 具体操作请查看test_yaml_config.cpp
 *
 * 加载yaml文件采用的是递归方式并且不支持数组, 当文件过大时请不要使用
 */

namespace eular {

using YamlValue = YAML::Node;

class YamlReader {
public:
    YamlReader();
    YamlReader(const std::string &path);
    ~YamlReader();

    void loadYaml(const std::string &path);

    template <typename T>
    T lookup(const std::string &key, const T &defaultVal = T())
    {
        T ret = defaultVal;
        rlock();
        auto it = mYamlConfigMap.find(key);
        if (it == mYamlConfigMap.end()) {
            goto __unlock;
        }

        try {
            ret = it->second.as<T>();
        } catch (const std::exception &e) {
            printf("%s\n", e.what());
        }

__unlock:
        runlock();
        return ret;
    }

    YamlValue at(const std::string &key);

    bool valid() { return isValid; }

    YamlValue root();

    void foreach(std::string &output, bool outValue = true);

protected:
    void loadYaml(const std::string &prefix, const YamlValue &node);
    void rlock();
    void runlock();

private:
    YAML::Node  mYamlRoot;
    std::string mYamlPath;
    void *      mMutex;
    std::map<std::string, YamlValue> mYamlConfigMap;
    bool isValid;
};

using YamlReaderInstance = Singleton<YamlReader>;
} // namespace eular

#endif // __CONFIG_YAML_CONFIG_H__