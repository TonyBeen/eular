#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include "catch/catch.hpp"

#include <config/json.h>
#include <config/observer.h>
#include <config/yaml.h>

#include <string>
#include <vector>
#include <algorithm>

namespace {
std::string TestDataFilePath(const std::string &fileName)
{
#ifdef CONFIG_TEST_DATA_DIR
    std::string dir = CONFIG_TEST_DATA_DIR;
#else
    std::string dir = "test";
#endif

    if (!dir.empty()) {
        const char tail = dir[dir.size() - 1];
        if (tail == '/' || tail == '\\') {
            return dir + fileName;
        }

        return dir + "/" + fileName;
    }

    return fileName;
}
} // namespace

TEST_CASE("ConfigObserver yaml full matching rules", "[observer][yaml][rules]")
{
    eular::YamlParser parser;
    REQUIRE(parser.load(TestDataFilePath("yaml_config.yaml")).code() == eular::CONFIG_OK);

    eular::ConfigObserver<eular::YamlParser, eular::YamlNode> observer;
    std::vector<std::string> events;

    REQUIRE(observer.subscribe("$", [&](const std::string &path, const eular::YamlNode &node) {
        REQUIRE(node.valid());
        CHECK(node.isMap());
        events.push_back("root:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$metadata", [&](const std::string &path, const eular::YamlNode &node) {
        REQUIRE(node.valid());
        CHECK(node.at("name").as<std::string>("") == "nginx-demo");
        events.push_back("exact:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$metadata.+", [&](const std::string &path, const eular::YamlNode &node) {
        REQUIRE(node.valid());
        events.push_back("one:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$metadata.#", [&](const std::string &path, const eular::YamlNode &node) {
        REQUIRE(node.valid());
        events.push_back("tree:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$spec.volum.containers[0].+", [&](const std::string &path, const eular::YamlNode &node) {
        REQUIRE(node.valid());
        events.push_back("array-one:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$spec.volum.containers[0].#", [&](const std::string &path, const eular::YamlNode &node) {
        REQUIRE(node.valid());
        events.push_back("array-tree:" + path);
    }) != 0);

    CHECK(observer.notify(parser, "$") == 1);
    CHECK(observer.notify(parser, "$metadata") == 2);
    CHECK(observer.notify(parser, "$metadata.name") == 2);
    CHECK(observer.notify(parser, "$metadata.labels.app") == 1);
    CHECK(observer.notify(parser, "$spec.volum.containers[0].image") == 2);
    CHECK(observer.notify(parser, "$spec.volum.containers[0]") == 1);

    CHECK(events.size() == 9);
    CHECK(events[0] == "root:$");
    CHECK(events[1] == "exact:$metadata");
    CHECK(events[2] == "tree:$metadata");
    CHECK(events[3] == "one:$metadata.name");
    CHECK(events[4] == "tree:$metadata.name");
    CHECK(events[5] == "tree:$metadata.labels.app");
    CHECK(events[6] == "array-one:$spec.volum.containers[0].image");
    CHECK(events[7] == "array-tree:$spec.volum.containers[0].image");
    CHECK(events[8] == "array-tree:$spec.volum.containers[0]");
}

TEST_CASE("ConfigObserver json full matching rules", "[observer][json][rules]")
{
    const std::string json = std::string(
        "{"
        "\"metadata\":{\"name\":\"json-demo\",\"labels\":{\"app\":\"api\"}},"
        "\"plugins\":{\"auth\":{\"enabled\":true,\"token\":{\"ttl\":3600}},\"metrics\":{\"enabled\":false}},"
        "\"items\":[{\"name\":\"first\",\"port\":8080},{\"name\":\"second\",\"port\":9090}]"
        "}");

    eular::JsonParser parser;
    REQUIRE(parser.loadFromString(json).code() == eular::CONFIG_OK);

    eular::ConfigObserver<eular::JsonParser, eular::JsonNode> observer;
    std::vector<std::string> events;

    REQUIRE(observer.subscribe("$plugins", [&](const std::string &path, const eular::JsonNode &node) {
        REQUIRE(node.valid());
        CHECK(node.isMap());
        events.push_back("exact:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$plugins.+", [&](const std::string &path, const eular::JsonNode &node) {
        REQUIRE(node.valid());
        CHECK(node.isMap());
        events.push_back("one:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$plugins.#", [&](const std::string &path, const eular::JsonNode &node) {
        REQUIRE(node.valid());
        events.push_back("tree:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$plugins.+.enabled", [&](const std::string &path, const eular::JsonNode &node) {
        REQUIRE(node.valid());
        CHECK(node.isScalar());
        events.push_back("enabled:" + path);
    }) != 0);

    REQUIRE(observer.subscribe("$items[0].+", [&](const std::string &path, const eular::JsonNode &node) {
        REQUIRE(node.valid());
        events.push_back("array-one:" + path);
    }) != 0);

    CHECK(observer.notify(parser, "$plugins") == 2);
    CHECK(observer.notify(parser, "$plugins.auth") == 2);
    CHECK(observer.notify(parser, "$plugins.auth.enabled") == 2);
    CHECK(observer.notify(parser, "$plugins.auth.token.ttl") == 1);
    CHECK(observer.notify(parser, "$items[0].name") == 1);

    CHECK(events.size() == 8);
    CHECK(events[0] == "exact:$plugins");
    CHECK(events[1] == "tree:$plugins");
    CHECK(events[2] == "one:$plugins.auth");
    CHECK(events[3] == "tree:$plugins.auth");
    CHECK(events[4] == "tree:$plugins.auth.enabled");
    CHECK(events[5] == "enabled:$plugins.auth.enabled");
    CHECK(events[6] == "tree:$plugins.auth.token.ttl");
    CHECK(events[7] == "array-one:$items[0].name");
}

TEST_CASE("ConfigObserver notify ignores invalid paths before loading nodes", "[observer][rules]")
{
    eular::YamlParser parser;
    REQUIRE(parser.load(TestDataFilePath("yaml_config.yaml")).code() == eular::CONFIG_OK);

    eular::ConfigObserver<eular::YamlParser, eular::YamlNode> observer;
    size_t calls = 0;
    REQUIRE(observer.subscribe("$metadata.#", [&](const std::string &, const eular::YamlNode &) {
        ++calls;
    }) != 0);

    CHECK(observer.notify(parser, "") == 0);
    CHECK(observer.notify(parser, "metadata") == 0);
    CHECK(observer.notify(parser, "$metadata.") == 0);
    CHECK(observer.notify(parser, "$metadata..name") == 0);
    CHECK(calls == 0);
}

TEST_CASE("ConfigObserver yaml auto discovery notify", "[observer][yaml][discover]")
{
    eular::YamlParser parser;
    REQUIRE(parser.load(TestDataFilePath("yaml_config.yaml")).code() == eular::CONFIG_OK);

    std::vector<std::string> paths = parser.paths();
    CHECK(std::find(paths.begin(), paths.end(), "$metadata") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "$metadata.name") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "$metadata.labels.app") != paths.end());

    eular::ConfigObserver<eular::YamlParser, eular::YamlNode> observer;
    std::vector<std::string> metadataPaths;

    REQUIRE(observer.subscribe("$metadata.#", [&](const std::string &path, const eular::YamlNode &node) {
        REQUIRE(node.valid());
        metadataPaths.push_back(path);
    }) != 0);

    CHECK(observer.notify(parser) == 4);
    CHECK(std::find(metadataPaths.begin(), metadataPaths.end(), "$metadata") != metadataPaths.end());
    CHECK(std::find(metadataPaths.begin(), metadataPaths.end(), "$metadata.name") != metadataPaths.end());
    CHECK(std::find(metadataPaths.begin(), metadataPaths.end(), "$metadata.labels") != metadataPaths.end());
    CHECK(std::find(metadataPaths.begin(), metadataPaths.end(), "$metadata.labels.app") != metadataPaths.end());
}

TEST_CASE("ConfigObserver json auto discovery notify", "[observer][json][discover]")
{
    const std::string json = std::string(
        "{"
        "\"metadata\":{\"name\":\"json-demo\",\"labels\":{\"app\":\"api\"}},"
        "\"plugins\":{\"auth\":{\"enabled\":true},\"metrics\":{\"enabled\":false}}"
        "}");

    eular::JsonParser parser;
    REQUIRE(parser.loadFromString(json).code() == eular::CONFIG_OK);

    std::vector<std::string> paths = parser.paths();
    CHECK(std::find(paths.begin(), paths.end(), "$plugins") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "$plugins.auth") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "$plugins.auth.enabled") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "$plugins.metrics.enabled") != paths.end());

    eular::ConfigObserver<eular::JsonParser, eular::JsonNode> observer;
    std::vector<std::string> enabledPaths;

    REQUIRE(observer.subscribe("$plugins.+.enabled", [&](const std::string &path, const eular::JsonNode &node) {
        REQUIRE(node.valid());
        REQUIRE(node.isScalar());
        enabledPaths.push_back(path);
    }) != 0);

    CHECK(observer.notify(parser) == 2);
    CHECK(std::find(enabledPaths.begin(), enabledPaths.end(), "$plugins.auth.enabled") != enabledPaths.end());
    CHECK(std::find(enabledPaths.begin(), enabledPaths.end(), "$plugins.metrics.enabled") != enabledPaths.end());
}

TEST_CASE("ConfigObserver trie matching with many subscriptions", "[observer][trie]")
{
    const std::string json = std::string(
        "{"
        "\"plugins\":{\"auth\":{\"enabled\":true,\"token\":{\"ttl\":3600}},\"metrics\":{\"enabled\":false}},"
        "\"database\":{\"host\":\"127.0.0.1\",\"port\":3306}"
        "}");

    eular::JsonParser parser;
    REQUIRE(parser.loadFromString(json).code() == eular::CONFIG_OK);

    eular::ConfigObserver<eular::JsonParser, eular::JsonNode> observer;
    size_t calls = 0;

    for (int i = 0; i < 20; ++i) {
        REQUIRE(observer.subscribe("$unused" + std::to_string(i) + ".#", [&](const std::string &, const eular::JsonNode &) {
            ++calls;
        }) != 0);
    }

    uint64_t pluginsTreeId = observer.subscribe("$plugins.#", [&](const std::string &path, const eular::JsonNode &node) {
        REQUIRE(node.valid());
        ++calls;
        if (path == "$plugins.auth.token.ttl") {
            CHECK(node.as<int>(0) == 3600);
        }
    });
    REQUIRE(pluginsTreeId != 0);

    REQUIRE(observer.subscribe("$plugins.+.enabled", [&](const std::string &, const eular::JsonNode &node) {
        REQUIRE(node.valid());
        CHECK(node.isScalar());
        ++calls;
    }) != 0);

    CHECK(observer.notify(parser, "$database.host") == 0);
    CHECK(calls == 0);

    CHECK(observer.notify(parser, "$plugins.auth.enabled") == 2);
    CHECK(calls == 2);

    CHECK(observer.notify(parser, "$plugins.auth.token.ttl") == 1);
    CHECK(calls == 3);

    REQUIRE(observer.unsubscribe(pluginsTreeId));
    CHECK(observer.notify(parser, "$plugins.metrics.enabled") == 1);
    CHECK(calls == 4);
}

TEST_CASE("ConfigObserver exact yaml subscription", "[observer][yaml]")
{
    eular::YamlParser parser;
    REQUIRE(parser.load(TestDataFilePath("yaml_config.yaml")).code() == eular::CONFIG_OK);

    eular::ConfigObserver<eular::YamlParser, eular::YamlNode> observer;

    size_t calls = 0;
    std::string receivedPath;
    uint64_t id = observer.subscribe("$metadata", [&](const std::string &path, const eular::YamlNode &node) {
        ++calls;
        receivedPath = path;
        REQUIRE(node.valid());
        REQUIRE(node.isMap());
        CHECK(node.at("name").as<std::string>("") == "nginx-demo");
        CHECK(node.at("labels").at("app").as<std::string>("") == "nginx");
    });

    REQUIRE(id != 0);
    CHECK(observer.notify(parser, "$metadata") == 1);
    CHECK(calls == 1);
    CHECK(receivedPath == "$metadata");

    CHECK(observer.notify(parser, "$metadata.name") == 0);
    CHECK(calls == 1);
}

TEST_CASE("ConfigObserver yaml wildcard subscription", "[observer][yaml][wildcard]")
{
    eular::YamlParser parser;
    REQUIRE(parser.load(TestDataFilePath("yaml_config.yaml")).code() == eular::CONFIG_OK);

    eular::ConfigObserver<eular::YamlParser, eular::YamlNode> observer;

    std::vector<std::string> metadataPaths;
    uint64_t metadataId = observer.subscribe("$metadata.#", [&](const std::string &path, const eular::YamlNode &node) {
        REQUIRE(node.valid());
        metadataPaths.push_back(path);
    });

    size_t selectorCalls = 0;
    uint64_t selectorId = observer.subscribe("$spec.+.app", [&](const std::string &path, const eular::YamlNode &node) {
        ++selectorCalls;
        CHECK(path == "$spec.selector.app");
        CHECK(node.as<std::string>("") == "nginx");
    });

    REQUIRE(metadataId != 0);
    REQUIRE(selectorId != 0);

    CHECK(observer.notify(parser, "$metadata") == 1);
    CHECK(observer.notify(parser, "$metadata.name") == 1);
    CHECK(observer.notify(parser, "$metadata.labels.app") == 1);
    CHECK(metadataPaths.size() == 3);

    CHECK(observer.notify(parser, "$spec.selector.app") == 1);
    CHECK(selectorCalls == 1);
    CHECK(observer.notify(parser, "$spec.volum.containers") == 0);
    CHECK(selectorCalls == 1);
}

TEST_CASE("ConfigObserver unsubscribe and invalid inputs", "[observer]")
{
    eular::YamlParser parser;
    REQUIRE(parser.load(TestDataFilePath("yaml_config.yaml")).code() == eular::CONFIG_OK);

    eular::ConfigObserver<eular::YamlParser, eular::YamlNode> observer;

    CHECK(observer.subscribe("", [](const std::string &, const eular::YamlNode &) {}) == 0);
    CHECK(observer.subscribe("metadata", [](const std::string &, const eular::YamlNode &) {}) == 0);
    CHECK(observer.subscribe("$metadata..name", [](const std::string &, const eular::YamlNode &) {}) == 0);
    CHECK(observer.subscribe("$metadata.#.name", [](const std::string &, const eular::YamlNode &) {}) == 0);
    CHECK(observer.subscribe("$metadata", eular::ConfigObserver<eular::YamlParser, eular::YamlNode>::Callback()) == 0);

    size_t calls = 0;
    uint64_t id = observer.subscribe("$metadata.#", [&](const std::string &, const eular::YamlNode &) {
        ++calls;
    });
    REQUIRE(id != 0);

    CHECK(observer.notify(parser, "$metadata.missing") == 0);
    CHECK(calls == 0);

    CHECK(observer.unsubscribe(id));
    CHECK_FALSE(observer.unsubscribe(id));
    CHECK(observer.notify(parser, "$metadata") == 0);
    CHECK(calls == 0);
}

TEST_CASE("ConfigObserver works with json parser and node", "[observer][json]")
{
    eular::JsonParser parser;
    REQUIRE(parser.loadFromString("{\"metadata\":{\"name\":\"json-demo\",\"labels\":{\"app\":\"api\"}}}").code() == eular::CONFIG_OK);

    eular::ConfigObserver<eular::JsonParser, eular::JsonNode> observer;

    size_t calls = 0;
    uint64_t id = observer.subscribe("$metadata.#", [&](const std::string &path, const eular::JsonNode &node) {
        ++calls;
        if (path == "$metadata.labels.app") {
            CHECK(node.as<std::string>("") == "api");
        }
    });

    REQUIRE(id != 0);
    CHECK(observer.notify(parser, "$metadata.labels.app") == 1);
    CHECK(calls == 1);
}
