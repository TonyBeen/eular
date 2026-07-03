#include <config/yaml.h>

#include <iostream>
#include <string>

namespace {
std::string buildYamlText()
{
    return std::string(
        "app:\n"
        "  name: demo\n"
        "  retry: 3\n"
        "  timeout: 1.5\n"
        "  enabled: true\n"
        "  tags:\n"
        "    - alpha\n"
        "    - beta\n"
        "  services:\n"
        "    - name: gateway\n"
        "      port: 8080\n"
        "      tls: yes\n"
        "    - name: worker\n"
        "      port: 9090\n"
        "      tls: no\n");
}
} // namespace

int main(int argc, char **argv)
{
    eular::YamlParser parser;

    eular::ConfigResult loadRes;
    if (argc > 1) {
        loadRes = parser.load(argv[1]);
    } else {
        std::string yamlText = buildYamlText();
        loadRes = parser.loadFromString(yamlText);
    }

    if (loadRes.code() != eular::CONFIG_OK) {
        std::cerr << "yaml load failed: code=" << loadRes.code() << ", msg=" << loadRes.message() << std::endl;
        return 1;
    }

    std::cout << "app.name=" << parser.getNode("$app.name").as<std::string>("unknown") << std::endl;
    std::cout << "app.retry=" << parser.getNode("$app.retry").as<int>(0) << std::endl;
    std::cout << "app.timeout=" << parser.getNode("$app.timeout").as<double>(0.0) << std::endl;
    std::cout << "app.enabled=" << parser.getNode("$app.enabled").as<bool>(false) << std::endl;

    eular::YamlNode tags = parser.getNode("$app.tags");
    std::cout << "app.tags(size=" << tags.size() << "):";
    for (uint32_t i = 0; i < tags.size(); ++i) {
        std::cout << " " << tags.at(i).as<std::string>("");
    }
    std::cout << std::endl;

    eular::YamlNode services = parser.getNode("$app.services");
    std::cout << "services(size=" << services.size() << ")" << std::endl;
    for (uint32_t i = 0; i < services.size(); ++i) {
        eular::YamlNode service = services.at(i);
        std::string name = service.at("name").as<std::string>("unknown");
        int port = service.at("port").as<int>(0);
        bool tls = service.at("tls").as<bool>(false);
        std::cout << "  - " << name << ":" << port << ", tls=" << tls << std::endl;
    }

    int missingFallback = parser.getNode("$app.not_exists").as<int>(12345);
    std::cout << "missing fallback=" << missingFallback << std::endl;

    eular::ConfigResult bad = parser.loadFromString("app: [1, 2");
    std::cout << "bad yaml result code=" << bad.code() << ", message=" << bad.message() << std::endl;

    return 0;
}
