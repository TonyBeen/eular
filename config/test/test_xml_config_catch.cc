#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include "catch/catch.hpp"

#include <config/xml.h>

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

std::string BasicXml()
{
    return std::string(
        "<root>"
        "  <server>"
        "    <host>127.0.0.1</host>"
        "    <port>8080</port>"
        "    <enabled>yes</enabled>"
        "    <ratio>3.25</ratio>"
        "  </server>"
        "  <log>"
        "    <level>info</level>"
        "  </log>"
        "</root>");
}
} // namespace

TEST_CASE("XmlConfig parse and lookup", "[xml][config][lookup]")
{
    eular::XmlConfig config;
    REQUIRE(config.parse(BasicXml()));

    CHECK(config.lookup<std::string>("root.server.host", "") == "127.0.0.1");
    CHECK(config.lookup<int>("root.server.port", 0) == 8080);
    CHECK(config.lookup<bool>("root.server.enabled", false) == true);
    CHECK(config.lookup<double>("root.server.ratio", 0.0) == Approx(3.25));
    CHECK(config.lookup<std::string>("root.log.level", "") == "info");

    CHECK(config.lookup<int>("root.not.exists", 9) == 9);
    config.foreach();
}

TEST_CASE("XmlConfig load file and conversion fallback", "[xml][config][file]")
{
    eular::XmlConfig config;
    REQUIRE(config.loadFile(TestDataFilePath("xml_config.xml")));

    CHECK(config.lookup<std::string>("root.server.host", "") == "192.168.1.100");
    CHECK(config.lookup<int>("root.server.port", 0) == 9000);
    CHECK(config.lookup<bool>("root.server.enabled", false) == false);
    CHECK(config.lookup<int>("root.server.bad_int", 88) == 88);
}

TEST_CASE("XmlConfig error branches", "[xml][config][error]")
{
    eular::XmlConfig config;

    CHECK_FALSE(config.loadFile(TestDataFilePath("not_exists_abcdef.xml")));
    CHECK_FALSE(config.parse("<root><item>1</root>"));
}
