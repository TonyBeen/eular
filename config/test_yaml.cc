/*************************************************************************
    > File Name: test_yaml.cc
    > Author: eular
    > Brief:
    > Created Time: Fri 13 Mar 2026 04:15:10 PM CST
 ************************************************************************/

#include <config/yaml.h>

int main(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : "./config.yaml";
    eular::YamlParser parser;
    auto result = parser.load(path);
    if (result.code() != eular::ConfigCode::CONFIG_OK) {
        printf("Failed to load config file: %s\n", result.message());
        return 1;
    }

    parser.foreachNode();
    return 0;
}
