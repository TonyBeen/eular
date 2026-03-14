/*************************************************************************
    > File Name: yaml.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 10 Mar 2026 03:58:21 PM CST
 ************************************************************************/

#include "config/yaml.h"

#include <stdio.h>
#include <unordered_map>

#include <yaml.h>

#include "config/yaml/node_private.hpp"

namespace eular {
struct Frame {
    int32_t         id;
    std::string     path;
};

struct YamlParserPrivate
{
    YamlParserPrivate(const YamlParserPrivate &) = delete;
    YamlParserPrivate &operator=(const YamlParserPrivate &) = delete;

    yaml_document_t     document;
    bool                initialized = false;
    std::unordered_map<std::string, int32_t> pathIdMap;

    YamlParserPrivate()
    {
        memset(&document, 0, sizeof(document));
    }

    void clear() noexcept
    {
        if (initialized) {
            yaml_document_delete(&document);
            initialized = false;
        }

        pathIdMap.clear();
    }

    ~YamlParserPrivate()
    {
        clear();
    }
};

YamlParser::YamlParser()
{
    m_private = std::make_shared<YamlParserPrivate>();
}

YamlParser::~YamlParser()
{
}

ConfigResult YamlParser::load(const char *filePath) noexcept
{
    ConfigResult result;
    std::string fileContent = readFromeFile(filePath, result);
    if (result.code() != ConfigCode::CONFIG_OK) {
        return result;
    }

    return loadFromString(fileContent.c_str(), fileContent.size());
}

ConfigResult YamlParser::load(const std::string &filePath) noexcept
{
    return load(filePath.c_str());
}

ConfigResult YamlParser::loadFromString(const char *yamlContent, uint32_t size) noexcept
{
    ConfigResult result;
    if (checkPrivateFailed(result)) {
        return result;
    }

    if (m_private->initialized) {
        m_errorMsg = "yaml document has been loaded";
        return ConfigResult(ConfigCode::CONFIG_ALREADY_LOADED, m_errorMsg.c_str());
    }

    if (yamlContent == nullptr || size == 0) {
        result = ConfigResult(ConfigCode::CONFIG_INVALID_ARGUMENT, "yamlContent is null or size is zero");
        return result;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        m_errorMsg = "failed to initialize yaml parser";
        result = ConfigResult(ConfigCode::CONFIG_INIT_ERROR, m_errorMsg.c_str());
        return result;
    }
    if (size == UINT32_MAX) {
        size = strlen(yamlContent);
    }

    yaml_parser_set_input_string(&parser, (const uint8_t*)(yamlContent), size);
    if (yaml_parser_load(&parser, &m_private->document) == 0) {
        m_errorMsg = "failed to parse yaml content: " + (parser.problem != nullptr ? std::string(parser.problem) : std::to_string(parser.error));
        result = ConfigResult(ConfigCode::CONFIG_PARSE_ERROR, m_errorMsg.c_str());
        yaml_parser_delete(&parser);
        return result;
    }
    yaml_parser_delete(&parser);

    buildMap(result);
    if (result.code() == ConfigCode::CONFIG_OK) {
        m_private->initialized = true;
    }
    return result;
}

ConfigResult YamlParser::loadFromString(const std::string &yamlContent) noexcept
{
    return loadFromString(yamlContent.c_str(), yamlContent.size());
}

void YamlParser::reset()
{
    if (m_private != nullptr) {
        m_private->clear();
    }
}

YamlNode YamlParser::getNode(const std::string &key) const noexcept
{
    if (m_private == nullptr || !m_private->initialized) {
        return YamlNode();
    }

    auto it = m_private->pathIdMap.find(key);
    if (it == m_private->pathIdMap.end()) {
        return YamlNode();
    }

    YamlNode node;
    std::shared_ptr<YamlNodePrivate> nodePrivate = std::make_shared<YamlNodePrivate>();
    nodePrivate->document = &m_private->document;
    nodePrivate->node = yaml_document_get_root_node(nodePrivate->document);
    nodePrivate->id = static_cast<int32_t>(nodePrivate->node - nodePrivate->document->nodes.start) + 1;
    node.m_private = nodePrivate;
    return node;
}

void YamlParser::foreachNode() const noexcept
{
    if (m_private == nullptr || !m_private->initialized) {
        return;
    }

    for (const auto &pair : m_private->pathIdMap) {
        printf("'%s' <===> %d\n", pair.first.c_str(), pair.second);
    }
}

std::string YamlParser::readFromeFile(const char *filePath, ConfigResult &result)
{
    std::string fileContent;
    if (filePath == nullptr) {
        result = ConfigResult(ConfigCode::CONFIG_INVALID_ARGUMENT, "filePath is null");
        return fileContent;
    }
    FILE* file = fopen(filePath, "r");
    if (file == nullptr) {
        m_errorMsg = "failed to open file: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_NOT_FOUND, m_errorMsg.c_str());
        return fileContent;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        m_errorMsg = "failed to seek file: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_OP_ERROR, m_errorMsg.c_str());
        fclose(file);
        return fileContent;
    }
    int32_t fileSize = ftell(file);
    if (fileSize < 0) {
        m_errorMsg = "failed to get file size: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_OP_ERROR, m_errorMsg.c_str());
        fclose(file);
        return fileContent;
    }
    if (fileSize == 0) {
        m_errorMsg = "file is empty: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_EMPTY, m_errorMsg.c_str());
        fclose(file);
        return fileContent;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        m_errorMsg = "failed to seek file: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_OP_ERROR, m_errorMsg.c_str());
        fclose(file);
        return fileContent;
    }

    fileContent.resize(fileSize);
    size_t readSize = fread(&fileContent[0], 1, fileSize, file);
    if (readSize != static_cast<size_t>(fileSize)) {
        m_errorMsg = "failed to read file: " + std::string(filePath);
        result = ConfigResult(ConfigCode::CONFIG_FILE_OP_ERROR, m_errorMsg.c_str());
        fclose(file);
        return fileContent;
    }
    fclose(file);
    return fileContent;
}

bool YamlParser::checkPrivateFailed(ConfigResult &result) noexcept
{
    if (m_private == nullptr) {
        try {
            m_private = std::make_shared<YamlParserPrivate>();
        } catch(const std::exception& e) {
            m_errorMsg = "failed to allocate memory for YamlParserPrivate: " + std::string(e.what());
            result = ConfigResult(ConfigCode::CONFIG_NO_MEMORY, m_errorMsg.c_str());
        }
    }

    return m_private == nullptr;
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

void YamlParser::buildMap(ConfigResult &result)
{
    m_private->pathIdMap.clear();
    yaml_node_t *node = yaml_document_get_root_node(&m_private->document);
    if (node == nullptr) {
        return;
    }

    int32_t rootId = static_cast<int32_t>(node - m_private->document.nodes.start) + 1;

    std::vector<Frame> frames;
    frames.push_back({rootId, "$"});

    while (!frames.empty()) {
        Frame current = std::move(frames.back());
        frames.pop_back();

        yaml_node_t *currentNode = yaml_document_get_node(&m_private->document, current.id);
        if (currentNode == nullptr) {
            continue;
        }

        if (!current.path.empty()) {
            m_private->pathIdMap.emplace(current.path, current.id);
        }

        if (currentNode->type == YAML_MAPPING_NODE) {
            std::vector<yaml_node_pair_t> pairs;
            pairs.reserve((size_t)(currentNode->data.mapping.pairs.top - currentNode->data.mapping.pairs.start));
            for (yaml_node_pair_t* p = currentNode->data.mapping.pairs.start; p < currentNode->data.mapping.pairs.top; ++p) {
                pairs.push_back(*p);
            }

            for (int32_t i = (int32_t)pairs.size() - 1; i >= 0; --i) {
                yaml_node_t *keyNode = yaml_document_get_node(&m_private->document, pairs[i].key);
                // 不支持复杂键, 只支持字符串键, 如下
                // ? {x: 1, y: 2}
                // : value2
                if (keyNode == nullptr || keyNode->type != YAML_SCALAR_NODE) {
                    m_errorMsg = "unsupported complex key in mapping node: " + current.path;
                    result = ConfigResult(ConfigCode::CONFIG_UNSUPPORTED, m_errorMsg.c_str());
                    m_private->pathIdMap.clear();
                    return;
                }

                frames.push_back({pairs[(size_t)i].value, JoinKey(current.path, (const char *)keyNode->data.scalar.value, keyNode->data.scalar.length)});
            }
        } else if (currentNode->type == YAML_SEQUENCE_NODE) {
            static auto joinIndex = [] (const std::string &base, int32_t idx) -> std::string {
                return base + "[" + std::to_string(idx) + "]";
            };

            int32_t nitems = (int32_t)(currentNode->data.sequence.items.top - currentNode->data.sequence.items.start);
            for (int32_t i = nitems - 1; i >= 0; --i) {
                int32_t childId = currentNode->data.sequence.items.start[i];
                frames.push_back({childId, joinIndex(current.path, i)});
            }
        }
    }
}
} // namespace eular
