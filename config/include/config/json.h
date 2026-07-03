/*************************************************************************
    > File Name: json.h
    > Author: eular
    > Brief:
    > Created Time: Tue 17 Mar 2026 04:30:00 PM CST
 ************************************************************************/

#ifndef __CONFIG_JSON_H__
#define __CONFIG_JSON_H__

#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

#include <config/result.h>
#include <config/exports.h>
#include <config/json/node.h>

namespace eular {
struct JsonParserPrivate;
class CONFIG_API JsonParser
{
    JsonParser(const JsonParser&) = delete;
    JsonParser& operator=(const JsonParser&) = delete;
    JsonParser(JsonParser&&) = delete;
    JsonParser& operator=(JsonParser&&) = delete;
public:
    using SP    = std::shared_ptr<JsonParser>;
    using WP    = std::weak_ptr<JsonParser>;
    using Ptr   = std::unique_ptr<JsonParser>;

    JsonParser();
    ~JsonParser();

    ConfigResult    load(const char *filePath) noexcept;
    ConfigResult    load(const std::string &filePath) noexcept;
    ConfigResult    loadFromString(const char *jsonContent, uint32_t size = UINT32_MAX) noexcept;
    ConfigResult    loadFromString(const std::string &jsonContent) noexcept;

    void            reset();

    JsonNode        getNode(const std::string &key) const noexcept;
    std::vector<std::string> paths() const noexcept;
    void            foreachPath(const std::function<void(const std::string &, const std::vector<std::string> &)> &visitor) const;
    void            foreachNode(const std::function<void(const std::string &, const JsonNode &)> &visitor) const;
    void            foreachNode(const std::function<void(const std::string &, const std::vector<std::string> &, const JsonNode &)> &visitor) const;

private:
    std::string readFromFile(const char *filePath, ConfigResult &result);
    ConfigResult loadFromStringInternal(const char *jsonContent, uint32_t size) noexcept;
    void buildMap(JsonParserPrivate &snapshot, ConfigResult &result);

private:
    std::shared_ptr<JsonParserPrivate> m_private = nullptr;
};

} // namespace eular

#endif // __CONFIG_JSON_H__
