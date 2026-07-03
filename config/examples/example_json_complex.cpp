#include <config/json.h>

#include <iostream>
#include <string>

namespace {
std::string buildJsonText()
{
    return std::string(
        "{"
        "\"name\":\"demo\","
        "\"retry\":4,"
        "\"timeout\":2.25,"
        "\"enabled\":true,"
        "\"labels\":[\"alpha\",\"beta\"],"
        "\"services\":["
        "  {\"name\":\"gateway\",\"port\":8080,\"tls\":true},"
        "  {\"name\":\"worker\",\"port\":9090,\"tls\":false}"
        "]"
        "}");
}
} // namespace

int main(int argc, char **argv)
{
    eular::JsonParser parser;

    eular::ConfigResult loadRes;
    if (argc > 1) {
        loadRes = parser.load(argv[1]);
    } else {
        std::string jsonText = buildJsonText();
        loadRes = parser.loadFromString(jsonText);
    }

    if (loadRes.code() != eular::CONFIG_OK) {
        std::cerr << "json load failed: code=" << loadRes.code() << ", msg=" << loadRes.message() << std::endl;
        return 1;
    }

    std::cout << "name=" << parser.getNode("$name").as<std::string>("unknown") << std::endl;
    std::cout << "retry=" << parser.getNode("$retry").as<int>(0) << std::endl;
    std::cout << "timeout=" << parser.getNode("$timeout").as<double>(0.0) << std::endl;
    std::cout << "enabled=" << parser.getNode("$enabled").as<bool>(false) << std::endl;

    eular::JsonNode labels = parser.getNode("$labels");
    std::cout << "labels(size=" << labels.size() << "):";
    for (uint32_t i = 0; i < labels.size(); ++i) {
        std::cout << " " << labels.at(i).as<std::string>("");
    }
    std::cout << std::endl;

    eular::JsonNode services = parser.getNode("$services");
    std::cout << "services(size=" << services.size() << ")" << std::endl;
    for (uint32_t i = 0; i < services.size(); ++i) {
        eular::JsonNode service = services.at(i);
        std::string name = service.at("name").as<std::string>("unknown");
        int port = service.at("port").as<int>(0);
        bool tls = service.at("tls").as<bool>(false);
        std::cout << "  - " << name << ":" << port << ", tls=" << tls << std::endl;
    }

    int missingFallback = parser.getNode("$not_exists").as<int>(54321);
    std::cout << "missing fallback=" << missingFallback << std::endl;

    eular::ConfigResult bad = parser.loadFromString("{\"name\":\"demo\"");
    std::cout << "bad json result code=" << bad.code() << ", message=" << bad.message() << std::endl;

    return 0;
}
