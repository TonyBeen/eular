#include <config/json.h>
#include <config/observer.h>
#include <config/yaml.h>

#include <iostream>
#include <string>

namespace {
std::string buildYamlText()
{
    return std::string(
        "metadata:\n"
        "  labels:\n"
        "    app: nginx\n"
        "  name: nginx-demo\n"
        "plugins:\n"
        "  auth:\n"
        "    enabled: true\n"
        "  metrics:\n"
        "    enabled: false\n");
}

std::string buildJsonText()
{
    return std::string(
        "{"
        "\"metadata\":{\"name\":\"json-demo\",\"labels\":{\"app\":\"api\"}},"
        "\"plugins\":{\"auth\":{\"enabled\":true},\"metrics\":{\"enabled\":false}}"
        "}");
}
} // namespace

int main()
{
    eular::YamlParser yamlParser;
    eular::ConfigResult yamlRes = yamlParser.loadFromString(buildYamlText());
    if (yamlRes.code() != eular::CONFIG_OK) {
        std::cerr << "yaml load failed: " << yamlRes.message() << std::endl;
        return 1;
    }

    eular::ConfigObserver<eular::YamlParser, eular::YamlNode> yamlObserver;

    uint64_t metadataId = yamlObserver.subscribe("$metadata", [](const std::string &path, const eular::YamlNode &node) {
        std::cout << "[yaml exact] " << path
                  << " name=" << node.at("name").as<std::string>("")
                  << " app=" << node.at("labels").at("app").as<std::string>("")
                  << std::endl;
    });

    yamlObserver.subscribe("$plugins.+", [](const std::string &path, const eular::YamlNode &node) {
        std::cout << "[yaml one-level] " << path
                  << " enabled=" << node.at("enabled").as<bool>(false)
                  << std::endl;
    });

    yamlObserver.subscribe("$plugins.#", [](const std::string &path, const eular::YamlNode &node) {
        std::cout << "[yaml tree] " << path;
        if (node.isScalar()) {
            std::cout << " value=" << node.scalar();
        }
        std::cout << std::endl;
    });

    std::cout << "auto notify yaml" << std::endl;
    size_t callbacks = yamlObserver.notify(yamlParser);
    std::cout << "callbacks=" << callbacks << std::endl;

    std::cout << "manual notify yaml $plugins.auth" << std::endl;
    callbacks = yamlObserver.notify(yamlParser, "$plugins.auth");
    std::cout << "callbacks=" << callbacks << std::endl;

    yamlObserver.unsubscribe(metadataId);
    std::cout << "notify yaml $metadata after unsubscribe" << std::endl;
    callbacks = yamlObserver.notify(yamlParser, "$metadata");
    std::cout << "callbacks=" << callbacks << std::endl;

    eular::JsonParser jsonParser;
    eular::ConfigResult jsonRes = jsonParser.loadFromString(buildJsonText());
    if (jsonRes.code() != eular::CONFIG_OK) {
        std::cerr << "json load failed: " << jsonRes.message() << std::endl;
        return 1;
    }

    eular::ConfigObserver<eular::JsonParser, eular::JsonNode> jsonObserver;
    jsonObserver.subscribe("$metadata.#", [](const std::string &path, const eular::JsonNode &node) {
        std::cout << "[json tree] " << path;
        if (node.isScalar()) {
            std::cout << " value=" << node.scalar();
        }
        std::cout << std::endl;
    });

    std::cout << "auto notify json" << std::endl;
    callbacks = jsonObserver.notify(jsonParser);
    std::cout << "callbacks=" << callbacks << std::endl;

    return 0;
}
