#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include "catch/catch.hpp"

#include <config/ini.h>

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
} // namespace

TEST_CASE("IniConfig parse/value/lookup", "[ini][config][lookup]")
{
    eular::IniConfig config;
    REQUIRE(config.parser(TestDataFilePath("ini_config.ini")));

    CHECK(config.value("host") == "127.0.0.1");
    CHECK(config.value("node1.host") == "192.168.10.12");
    CHECK(config.value("node2.host") == "192.168.10.120");

    CHECK(config.lookup<int>("port", -1) == 2000);
    CHECK(config.lookup<int>("node1.port", -1) == 3000);
    CHECK(config.lookup<std::string>("node1.host", "") == "192.168.10.12");
    CHECK(config.lookup<int>("missing.key", 99) == 99);
}

TEST_CASE("IniConfig c4 conversion and update", "[ini][config][convert]")
{
    eular::IniConfig config;
    REQUIRE(config.parser(TestDataFilePath("ini_config.ini")));

    config["flags.true"] = "true";
    config["flags.false"] = "NO";
    config["flags.one"] = "1";
    config["flags.zero"] = "0";
    config["number.value"] = "42";
    config["number.pi"] = "3.1415";
    config["text.bad"] = "abc";

    CHECK(config.lookup<bool>("flags.true", false) == true);
    CHECK(config.lookup<bool>("flags.false", true) == false);
    CHECK(config.lookup<bool>("flags.one", false) == true);
    CHECK(config.lookup<bool>("flags.zero", true) == false);

    CHECK(config.lookup<int>("number.value", 0) == 42);
    CHECK(config.lookup<double>("number.pi", 0.0) == Approx(3.1415));
    CHECK(config.lookup<int>("text.bad", 77) == 77);

    config["node1.host"] = "10.0.0.8";
    CHECK(config.value("node1.host") == "10.0.0.8");

    REQUIRE(config.del("node1.host"));
    CHECK(config.value("node1.host").empty());
}

TEST_CASE("IniConfig error branches", "[ini][config][error]")
{
    eular::IniConfig config;

    CHECK_FALSE(config.parser(TestDataFilePath("not_exists_abcdef.ini")));
    CHECK_FALSE(config.parser(TestDataFilePath("invalid_ini.ini")));
}
