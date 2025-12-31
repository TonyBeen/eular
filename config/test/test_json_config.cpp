/*************************************************************************
    > File Name: test_json_config.cpp
    > Author: hsz
    > Brief:
    > Created Time: Wed 04 Jan 2023 10:07:03 AM CST
 ************************************************************************/

#include <iostream>
#include <gtest/gtest.h>
#include <fstream>

#include "nlohmann/json.hpp"

using namespace std;

static std::string file = "./json_config.json";

TEST(JsonConfigTest, testRead) {
    nlohmann::json jsonValueRoot = nlohmann::json::parse(file);
    EXPECT_FALSE(jsonValueRoot.is_null());

    EXPECT_TRUE(jsonValueRoot["key1"].is_null());
    EXPECT_EQ(jsonValueRoot["key2"].get<int>(), 7);
    EXPECT_EQ(jsonValueRoot["key3"].get<double>(), 3.1415);
    EXPECT_EQ(jsonValueRoot["key4"].get<std::string>(), "value");
    EXPECT_TRUE(jsonValueRoot["key5"].get<bool>());
    EXPECT_TRUE(jsonValueRoot["friends"].is_array());
    EXPECT_TRUE(jsonValueRoot["players"].is_object());

    nlohmann::json &friendValue = jsonValueRoot["friends"];
    for (int i = 0; i < friendValue.size(); ++i) {
        std::cout << friendValue[i].get<std::string>() << std::endl;
    }

    nlohmann::json &playerValue = jsonValueRoot["players"];
    EXPECT_EQ(playerValue["one"], "Kante");
    EXPECT_EQ(playerValue["two"], "Hazard");
    auto it = playerValue.begin();
    for (; it != playerValue.end(); ++it) {
        std::cout << it.key() << ", " << it.value().get<std::string>() << std::endl;
    }
}

TEST(JsonConfigTest, test_write) {
    nlohmann::json jsonValueRoot;
    jsonValueRoot["key1"] = nlohmann::json(); // nullValue
    jsonValueRoot["key2"] = 7;
    jsonValueRoot["key3"] = 3.1415;
    jsonValueRoot["key4"] = "value";
    jsonValueRoot["key5"] = true;

    nlohmann::json array; // eular::JsonValue array(eular::JsonValueType::arrayValue);
    array.push_back("Dammy");
    array.push_back("Jack");
    jsonValueRoot["friends"] = array;

    nlohmann::json object; // eular::JsonValue object(eular::JsonValueType::objectValue);
    object["one"] = "Kante";
    object["two"] = "Hazard";
    jsonValueRoot["players"] = object;

    std::cout << "FastWriter: \n" << jsonValueRoot.dump() << std::endl;
    std::cout << "StyledWriter: \n" << jsonValueRoot.dump(4) << std::endl;
    /**
     * {
     *    "friends" : [ "Dammy", "Jack" ],
     *    "key1" : null,
     *    "key2" : 7,
     *    "key3" : 3.1415000000000002,
     *    "key4" : "value",
     *    "key5" : true,
     *    "players" : {
     *        "one" : "Kante",
     *        "two" : "Hazard"
     *    }
     * }
     * 
     */
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        file = argv[1];
    }

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
