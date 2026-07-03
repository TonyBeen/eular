/*************************************************************************
    > File Name: yaml.h
    > Author: eular
    > Brief:
    > Created Time: Tue 10 Mar 2026 03:41:48 PM CST
 ************************************************************************/

#ifndef __CONFIG_YAML_H__
#define __CONFIG_YAML_H__

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <functional>

#include <c4/charconv.hpp>

#include <config/result.h>
#include <config/exports.h>
#include <config/yaml/node.h>

namespace eular {
struct YamlParserPrivate;
class CONFIG_API YamlParser
{
    YamlParser(const YamlParser&) = delete;
    YamlParser& operator=(const YamlParser&) = delete;
    YamlParser(YamlParser&&) = delete;
    YamlParser& operator=(YamlParser&&) = delete;
public:
    using SP    = std::shared_ptr<YamlParser>;
    using WP    = std::weak_ptr<YamlParser>;
    using Ptr   = std::unique_ptr<YamlParser>;

    YamlParser();
    ~YamlParser();

    ConfigResult    load(const char *filePath) noexcept;
    ConfigResult    load(const std::string &filePath) noexcept;
    ConfigResult    loadFromString(const char *yamlContent, uint32_t size = UINT32_MAX) noexcept;
    ConfigResult    loadFromString(const std::string &yamlContent) noexcept;

    void            reset();

    YamlNode        getNode(const std::string &key) const noexcept;
    std::vector<std::string> paths() const noexcept;
    void            foreachPath(const std::function<void(const std::string &, const std::vector<std::string> &)> &visitor) const;
    void            foreachNode(const std::function<void(const std::string &, const YamlNode &)> &visitor) const;
    void            foreachNode(const std::function<void(const std::string &, const std::vector<std::string> &, const YamlNode &)> &visitor) const;

    void            foreachNode() const noexcept;

private:
    std::string readFromeFile(const char *filePath, ConfigResult &result);
    ConfigResult loadFromStringInternal(const char *yamlContent, uint32_t size) noexcept;
    void buildMap(YamlParserPrivate &snapshot, ConfigResult &result);

private:
    std::shared_ptr<YamlParserPrivate> m_private = nullptr;
    mutable std::mutex m_writeMutex;
};

} // namespace eular

#endif // __CONFIG_YAML_H__
