#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include "catch/catch.hpp"

#include <config/json.h>

#include <string>

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

std::string BasicJson()
{
    return std::string(
        "{"
        "\"key1\":null,"
        "\"key2\":7,"
        "\"key3\":3.1415,"
        "\"key4\":\"value\","
        "\"key5\":true,"
        "\"friends\":[\"Dammy\",\"Jack\"],"
        "\"players\":{\"one\":\"Kante\",\"two\":\"Hazard\"}"
        "}");
}
} // namespace

TEST_CASE("JsonParser load/getNode/foreach full path", "[json][parser][node]")
{
    eular::JsonParser parser;
    std::string json = BasicJson();

    eular::ConfigResult loadFromStringRes = parser.loadFromString(json.c_str(), static_cast<uint32_t>(json.size()));
    REQUIRE(loadFromStringRes.code() == eular::CONFIG_OK);

    eular::ConfigResult loadFromStdStringRes = parser.loadFromString(json);
    REQUIRE(loadFromStdStringRes.code() == eular::CONFIG_OK);

    parser.foreachNode();

    eular::JsonNode root = parser.getNode("$");
    REQUIRE(root.valid());
    REQUIRE(root.isMap());
    REQUIRE(root.type() == eular::JsonNode::NodeType::Map);
    REQUIRE(root.size() >= 7);

    eular::JsonNode oneNode = parser.getNode("$players.one");
    REQUIRE(oneNode.valid());
    REQUIRE(oneNode.isScalar());
    CHECK(oneNode.scalar() == "Kante");

    eular::JsonNode friendsNode = parser.getNode("$friends");
    REQUIRE(friendsNode.valid());
    REQUIRE(friendsNode.isSequence());
    REQUIRE(friendsNode.size() == 2);
    CHECK(friendsNode.at(0).scalar() == "Dammy");
    CHECK(friendsNode.at(1).scalar() == "Jack");
    CHECK_FALSE(friendsNode.at(2).valid());

    CHECK_FALSE(root.at("unknown").valid());
    CHECK_FALSE(parser.getNode("$missing.path").valid());

    int key2 = 0;
    REQUIRE(parser.getNode("$key2").as(&key2));
    CHECK(key2 == 7);

    double key3 = 0.0;
    REQUIRE(parser.getNode("$key3").as(&key3));
    CHECK(key3 == Approx(3.1415));

    bool key5 = false;
    REQUIRE(parser.getNode("$key5").as(&key5));
    CHECK(key5 == true);

    std::string key4;
    REQUIRE(parser.getNode("$key4").as(&key4));
    CHECK(key4 == "value");

    CHECK(parser.getNode("$key4").as<int>(777) == 777);

    const std::string sampleJsonPath = TestDataFilePath("json_config.json");
    eular::ConfigResult loadFromPathRes1 = parser.load(sampleJsonPath.c_str());
    REQUIRE(loadFromPathRes1.code() == eular::CONFIG_OK);
    eular::ConfigResult loadFromPathRes2 = parser.load(sampleJsonPath);
    REQUIRE(loadFromPathRes2.code() == eular::CONFIG_OK);

    std::string two;
    REQUIRE(parser.getNode("$players.two").as(&two));
    CHECK(two == "Hazard");

    parser.reset();
    CHECK_FALSE(parser.getNode("$").valid());
    parser.foreachNode();

    eular::ConfigResult reloadAfterReset = parser.loadFromString(json);
    REQUIRE(reloadAfterReset.code() == eular::CONFIG_OK);
    REQUIRE(parser.getNode("$players.one").scalar() == "Kante");
}

TEST_CASE("JsonParser error branches", "[json][parser][error]")
{
    eular::JsonParser parser;

    eular::ConfigResult nullFileRes = parser.load(static_cast<const char *>(nullptr));
    REQUIRE(nullFileRes.code() == eular::CONFIG_INVALID_ARGUMENT);

    const std::string missingPath = TestDataFilePath("not_exists_abcdef.json");
    eular::ConfigResult notFoundRes = parser.load(missingPath.c_str());
    REQUIRE(notFoundRes.code() == eular::CONFIG_NOT_FOUND);

    const std::string emptyFilePath = TestDataFilePath("empty.json");
    eular::ConfigResult emptyFileRes = parser.load(emptyFilePath.c_str());
    REQUIRE(emptyFileRes.code() == eular::CONFIG_FILE_EMPTY);

    eular::ConfigResult nullJsonRes = parser.loadFromString(static_cast<const char *>(nullptr), 16);
    REQUIRE(nullJsonRes.code() == eular::CONFIG_INVALID_ARGUMENT);

    eular::ConfigResult zeroSizeRes = parser.loadFromString("{}", 0);
    REQUIRE(zeroSizeRes.code() == eular::CONFIG_INVALID_ARGUMENT);

    eular::ConfigResult parseErrorRes = parser.loadFromString("{\"x\":[1,2}");
    REQUIRE(parseErrorRes.code() == eular::CONFIG_PARSE_ERROR);
}

TEST_CASE("JsonParser sequential update", "[json][parser][single-thread]")
{
    eular::JsonParser parser;

    std::string doc1 = "{\"counter\":1,\"flag\":true,\"items\":[10,20]}";
    std::string doc2 = "{\"counter\":2,\"flag\":false,\"items\":[30,40]}";
    std::string doc3 = "{\"counter\":3,\"flag\":true,\"items\":[50,60]}";

    REQUIRE(parser.loadFromString(doc1).code() == eular::CONFIG_OK);
    REQUIRE(parser.getNode("$counter").as<int>(0) == 1);
    REQUIRE(parser.getNode("$items[0]").as<int>(0) == 10);

    REQUIRE(parser.loadFromString(doc2).code() == eular::CONFIG_OK);
    REQUIRE(parser.getNode("$counter").as<int>(0) == 2);
    REQUIRE(parser.getNode("$items[0]").as<int>(0) == 30);
    REQUIRE(parser.getNode("$flag").as<bool>(true) == false);

    REQUIRE(parser.loadFromString(doc3).code() == eular::CONFIG_OK);
    REQUIRE(parser.getNode("$counter").as<int>(0) == 3);
    REQUIRE(parser.getNode("$items[0]").as<int>(0) == 50);
    REQUIRE(parser.getNode("$flag").as<bool>(false) == true);
}
