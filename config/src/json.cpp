/*************************************************************************
    > File Name: json.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 17 Mar 2026 04:45:00 PM CST
 ************************************************************************/

#include "config/json.h"

#include <stdio.h>
#include <unordered_map>

#include <yyjson.h>

#include "config/json/node_private.hpp"

namespace eular {
struct Frame {
    yyjson_val*      node;
    std::string      path;
};

struct JsonParserPrivate
{
    JsonParserPrivate() = default;
    JsonParserPrivate(const JsonParserPrivate &) = delete;
    JsonParserPrivate &operator=(const JsonParserPrivate &) = delete;

    yyjson_doc*      document = nullptr;
    bool             initialized = false;
    std::unordered_map<std::string, yyjson_val*> pathNodeMap;
    std::unordered_map<std::string, std::vector<std::string>> pathTokensMap;

    void clear() noexcept
    {
        if (document != nullptr) {
            yyjson_doc_free(document);
            document = nullptr;
        }

        initialized = false;
        pathNodeMap.clear();
        pathTokensMap.clear();
    }

    ~JsonParserPrivate()
    {
        clear();
    }
};

JsonParser::JsonParser()
{
}

JsonParser::~JsonParser()
{
}

ConfigResult JsonParser::load(const char *filePath) noexcept
{
    ConfigResult result;
    std::string fileContent = readFromFile(filePath, result);
    if (result.code() != ConfigCode::CONFIG_OK) {
        return result;
    }

    return loadFromString(fileContent.c_str(), fileContent.size());
}

ConfigResult JsonParser::load(const std::string &filePath) noexcept
{
    return load(filePath.c_str());
}

ConfigResult JsonParser::loadFromString(const char *jsonContent, uint32_t size) noexcept
{
    return loadFromStringInternal(jsonContent, size);
}

ConfigResult JsonParser::loadFromString(const std::string &jsonContent) noexcept
{
    return loadFromString(jsonContent.c_str(), jsonContent.size());
}

ConfigResult JsonParser::loadFromStringInternal(const char *jsonContent, uint32_t size) noexcept
{
    if (jsonContent == nullptr || size == 0) {
        return ConfigResult(ConfigCode::CONFIG_INVALID_ARGUMENT, "jsonContent is null or size is zero");
    }

    if (size == UINT32_MAX) {
        size = static_cast<uint32_t>(strlen(jsonContent));
    }

    yyjson_read_err err;
    memset(&err, 0, sizeof(err));
    yyjson_doc* doc = yyjson_read_opts((char *)(void *)jsonContent,
                                       static_cast<size_t>(size),
                                       YYJSON_READ_NOFLAG,
                                       NULL,
                                       &err);
    if (doc == nullptr) {
        std::string errorText = "failed to parse json content";
        if (err.msg != nullptr) {
            errorText += ": ";
            errorText += err.msg;
            errorText += " at ";
            errorText += std::to_string(err.pos);
        }
        return ConfigResult(ConfigCode::CONFIG_PARSE_ERROR, errorText);
    }

    std::shared_ptr<JsonParserPrivate> nextSnapshot;
    try {
        nextSnapshot = std::make_shared<JsonParserPrivate>();
    } catch(const std::exception& e) {
        yyjson_doc_free(doc);
        const std::string errorMsg = "failed to allocate memory for JsonParserPrivate: " + std::string(e.what());
        return ConfigResult(ConfigCode::CONFIG_NO_MEMORY, errorMsg);
    }

    nextSnapshot->document = doc;

    ConfigResult result;
    buildMap(*nextSnapshot, result);
    if (result.code() != ConfigCode::CONFIG_OK) {
        return result;
    }

    nextSnapshot->initialized = true;
    m_private = std::move(nextSnapshot);
    return result;
}

void JsonParser::reset()
{
    m_private.reset();
}

JsonNode JsonParser::getNode(const std::string &key) const noexcept
{
    if (m_private == nullptr || !m_private->initialized) {
        return JsonNode();
    }

    auto it = m_private->pathNodeMap.find(key);
    if (it == m_private->pathNodeMap.end()) {
        return JsonNode();
    }

    JsonNode node;
    std::shared_ptr<JsonNodePrivate> nodePrivate = std::make_shared<JsonNodePrivate>();
    nodePrivate->snapshot = m_private;
    nodePrivate->document = m_private->document;
    nodePrivate->node = it->second;
    if (nodePrivate->node == nullptr) {
        return JsonNode();
    }

    node.m_private = nodePrivate;
    return node;
}

std::vector<std::string> JsonParser::paths() const noexcept
{
    std::vector<std::string> result;
    if (m_private == nullptr || !m_private->initialized) {
        return result;
    }

    result.reserve(m_private->pathNodeMap.size());
    for (const auto &pair : m_private->pathNodeMap) {
        result.push_back(pair.first);
    }

    return result;
}

void JsonParser::foreachPath(const std::function<void(const std::string &, const std::vector<std::string> &)> &visitor) const
{
    if (!visitor || m_private == nullptr || !m_private->initialized) {
        return;
    }

    for (const auto &pair : m_private->pathTokensMap) {
        visitor(pair.first, pair.second);
    }
}

void JsonParser::foreachNode(const std::function<void(const std::string &, const JsonNode &)> &visitor) const
{
    if (!visitor || m_private == nullptr || !m_private->initialized) {
        return;
    }

    for (const auto &pair : m_private->pathNodeMap) {
        JsonNode node;
        std::shared_ptr<JsonNodePrivate> nodePrivate = std::make_shared<JsonNodePrivate>();
        nodePrivate->snapshot = m_private;
        nodePrivate->document = m_private->document;
        nodePrivate->node = pair.second;
        if (nodePrivate->node == nullptr) {
            continue;
        }

        node.m_private = nodePrivate;
        visitor(pair.first, node);
    }
}

void JsonParser::foreachNode(const std::function<void(const std::string &, const std::vector<std::string> &, const JsonNode &)> &visitor) const
{
    if (!visitor || m_private == nullptr || !m_private->initialized) {
        return;
    }

    for (const auto &pair : m_private->pathNodeMap) {
        auto tokenIt = m_private->pathTokensMap.find(pair.first);
        if (tokenIt == m_private->pathTokensMap.end()) {
            continue;
        }

        JsonNode node;
        std::shared_ptr<JsonNodePrivate> nodePrivate = std::make_shared<JsonNodePrivate>();
        nodePrivate->snapshot = m_private;
        nodePrivate->document = m_private->document;
        nodePrivate->node = pair.second;
        if (nodePrivate->node == nullptr) {
            continue;
        }

        node.m_private = nodePrivate;
        visitor(pair.first, tokenIt->second, node);
    }
}

std::string JsonParser::readFromFile(const char *filePath, ConfigResult &result)
{
    std::string fileContent;
    if (filePath == nullptr) {
        result = ConfigResult(ConfigCode::CONFIG_INVALID_ARGUMENT, "filePath is null");
        return fileContent;
    }

    FILE* file = fopen(filePath, "r");
    if (file == nullptr) {
        const std::string errorMsg = "failed to open file: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_NOT_FOUND, errorMsg);
        return fileContent;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        const std::string errorMsg = "failed to seek file: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_OP_ERROR, errorMsg);
        fclose(file);
        return fileContent;
    }

    long fileSizeLong = ftell(file);
    if (fileSizeLong < 0) {
        const std::string errorMsg = "failed to get file size: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_OP_ERROR, errorMsg);
        fclose(file);
        return fileContent;
    }

    size_t fileSize = static_cast<size_t>(fileSizeLong);
    if (fileSize == 0) {
        const std::string errorMsg = "file is empty: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_EMPTY, errorMsg);
        fclose(file);
        return fileContent;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        const std::string errorMsg = "failed to seek file: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_OP_ERROR, errorMsg);
        fclose(file);
        return fileContent;
    }

    fileContent.resize(fileSize);
    size_t readSize = fread(&fileContent[0], 1, fileSize, file);
    if (readSize != fileSize) {
        const std::string errorMsg = "failed to read file: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_OP_ERROR, errorMsg);
        fclose(file);
        return fileContent;
    }

    fclose(file);
    return fileContent;
}

static std::string JoinKey(const std::string &base, const std::string &key)
{
    if (base.empty()) {
        return key;
    }
    if (base.size() == 1) { // $
        return base + key;
    }

    return base + "." + key;
}

static std::string JoinKey(const std::string &base, const char *key, size_t size)
{
    std::string path;
    path.reserve(base.size() + 1 + size);
    path.append(base);
    if (base.empty()) {
        path.append(key, size);
        return path;
    }
    if (path.size() == 1) { // $
        return path.append(key, size);
    }

    path.push_back('.');
    path.append(key, size);
    return path;
}

static std::vector<std::string> TokenizePath(const std::string &path)
{
    std::vector<std::string> tokens;
    if (path.empty() || path[0] != '$' || path.size() == 1) {
        return tokens;
    }

    size_t begin = 1;
    while (begin < path.size()) {
        size_t end = path.find('.', begin);
        if (end == std::string::npos) {
            end = path.size();
        }
        if (end > begin) {
            tokens.push_back(path.substr(begin, end - begin));
        }
        begin = end + 1;
    }

    return tokens;
}

void JsonParser::buildMap(JsonParserPrivate &snapshot, ConfigResult &result)
{
    snapshot.pathNodeMap.clear();
    snapshot.pathTokensMap.clear();

    yyjson_val *root = yyjson_doc_get_root(snapshot.document);
    if (root == nullptr) {
        result = ConfigResult(ConfigCode::CONFIG_PARSE_ERROR, "json root is null");
        return;
    }

    std::vector<Frame> frames;
    frames.push_back({root, "$"});

    while (!frames.empty()) {
        Frame current = std::move(frames.back());
        frames.pop_back();

        if (current.node == nullptr) {
            continue;
        }

        if (!current.path.empty()) {
            snapshot.pathNodeMap.emplace(current.path, current.node);
            snapshot.pathTokensMap.emplace(current.path, TokenizePath(current.path));
        }

        if (yyjson_is_obj(current.node)) {
            size_t idx, max;
            yyjson_val *key, *val;
            yyjson_obj_foreach(current.node, idx, max, key, val) {
                const char *keyText = yyjson_get_str(key);
                size_t keySize = yyjson_get_len(key);
                if (keyText == nullptr || keySize == 0) {
                    continue;
                }

                frames.push_back({val, JoinKey(current.path, keyText, keySize)});
            }
        } else if (yyjson_is_arr(current.node)) {
            size_t idx, max;
            yyjson_val *val;
            yyjson_arr_foreach(current.node, idx, max, val) {
                frames.push_back({val, current.path + "[" + std::to_string(idx) + "]"});
            }
        }
    }
}

} // namespace eular
