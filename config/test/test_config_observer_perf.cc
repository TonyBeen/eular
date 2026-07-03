#include <config/json.h>
#include <config/observer.h>
#include <config/yaml.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string BuildYaml(size_t pluginCount)
{
    std::ostringstream os;
    os << "metadata:\n";
    os << "  name: perf-yaml\n";
    os << "  labels:\n";
    os << "    app: observer\n";
    os << "plugins:\n";
    for (size_t i = 0; i < pluginCount; ++i) {
        os << "  plugin" << i << ":\n";
        os << "    enabled: " << ((i % 2) == 0 ? "true" : "false") << "\n";
        os << "    endpoint: 10.0." << (i % 255) << "." << ((i * 7) % 255) << "\n";
        os << "    retry: " << (i % 5) << "\n";
        os << "    timeout: " << (100 + (i % 900)) << "\n";
        os << "    token:\n";
        os << "      ttl: " << (3600 + i) << "\n";
        os << "      issuer: issuer-" << i << "\n";
        os << "    limits:\n";
        os << "      qps: " << (1000 + i) << "\n";
        os << "      burst: " << (2000 + i) << "\n";
        os << "    tags:\n";
        os << "      - alpha-" << i << "\n";
        os << "      - beta-" << i << "\n";
    }
    return os.str();
}

std::string BuildJson(size_t pluginCount)
{
    std::ostringstream os;
    os << "{";
    os << "\"metadata\":{\"name\":\"perf-json\",\"labels\":{\"app\":\"observer\"}},";
    os << "\"plugins\":{";
    for (size_t i = 0; i < pluginCount; ++i) {
        if (i != 0) {
            os << ",";
        }
        os << "\"plugin" << i << "\":{";
        os << "\"enabled\":" << ((i % 2) == 0 ? "true" : "false") << ",";
        os << "\"endpoint\":\"10.1." << (i % 255) << "." << ((i * 7) % 255) << "\",";
        os << "\"retry\":" << (i % 5) << ",";
        os << "\"timeout\":" << (100 + (i % 900)) << ",";
        os << "\"token\":{\"ttl\":" << (3600 + i) << ",\"issuer\":\"issuer-" << i << "\"},";
        os << "\"limits\":{\"qps\":" << (1000 + i) << ",\"burst\":" << (2000 + i) << "},";
        os << "\"tags\":[\"alpha-" << i << "\",\"beta-" << i << "\"]";
        os << "}";
    }
    os << "}}";
    return os.str();
}

template<typename Parser, typename Node>
bool RegisterSubscriptions(eular::ConfigObserver<Parser, Node> &observer, size_t &callbackCount)
{
    const char *patterns[] = {
        "$metadata",
        "$metadata.#",
        "$plugins.+",
        "$plugins.+.enabled",
        "$plugins.+.token.#",
        "$plugins.+.limits.+",
        "$plugins.+.tags[0]",
        "$plugins.#"
    };

    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
        uint64_t id = observer.subscribe(patterns[i], [&callbackCount](const std::string &, const Node &node) {
            if (node.valid()) {
                ++callbackCount;
            }
        });
        if (id == 0) {
            return false;
        }
    }

    return true;
}

template<typename Parser, typename Node>
bool RegisterLargeSubscriptions(eular::ConfigObserver<Parser, Node> &observer, size_t &callbackCount)
{
    for (size_t i = 0; i < 48; ++i) {
        std::ostringstream pattern;
        pattern << "$plugins.plugin" << i << ".enabled";
        uint64_t id = observer.subscribe(pattern.str(), [&callbackCount](const std::string &, const Node &node) {
            if (node.valid()) {
                ++callbackCount;
            }
        });
        if (id == 0) {
            return false;
        }
    }

    for (size_t i = 0; i < 48; ++i) {
        std::ostringstream pattern;
        pattern << "$plugins.plugin" << i << ".token.#";
        uint64_t id = observer.subscribe(pattern.str(), [&callbackCount](const std::string &, const Node &node) {
            if (node.valid()) {
                ++callbackCount;
            }
        });
        if (id == 0) {
            return false;
        }
    }

    for (size_t i = 0; i < 48; ++i) {
        std::ostringstream pattern;
        pattern << "$plugins.plugin" << i << ".limits.+";
        uint64_t id = observer.subscribe(pattern.str(), [&callbackCount](const std::string &, const Node &node) {
            if (node.valid()) {
                ++callbackCount;
            }
        });
        if (id == 0) {
            return false;
        }
    }

    const char *sharedPatterns[] = {
        "$plugins.+.enabled",
        "$plugins.+.endpoint",
        "$plugins.+.retry",
        "$plugins.+.timeout",
        "$plugins.+.token.ttl",
        "$plugins.+.token.issuer",
        "$plugins.+.limits.qps",
        "$plugins.+.limits.burst",
        "$plugins.+.tags[0]",
        "$plugins.+.tags[1]",
        "$plugins.+.token.#",
        "$plugins.+.limits.+",
        "$plugins.plugin10.#",
        "$plugins.plugin20.#",
        "$plugins.plugin30.#",
        "$plugins.plugin40.#",
        "$metadata.#",
        "$plugins.#",
        "$plugins.+",
        "$metadata"
    };

    for (size_t i = 0; i < sizeof(sharedPatterns) / sizeof(sharedPatterns[0]); ++i) {
        uint64_t id = observer.subscribe(sharedPatterns[i], [&callbackCount](const std::string &, const Node &node) {
            if (node.valid()) {
                ++callbackCount;
            }
        });
        if (id == 0) {
            return false;
        }
    }

    return RegisterSubscriptions(observer, callbackCount);
}

template<typename Parser, typename Node>
void RunNotifyBenchmark(const char *name, Parser &parser, size_t configBytes, size_t rounds, bool largeSubscriptions)
{
    eular::ConfigObserver<Parser, Node> observer;
    size_t callbackCount = 0;
    bool registerOk = largeSubscriptions
        ? RegisterLargeSubscriptions(observer, callbackCount)
        : RegisterSubscriptions(observer, callbackCount);
    if (!registerOk) {
        std::cerr << name << " failed to register subscriptions" << std::endl;
        std::exit(1);
    }

    const std::vector<std::string> paths = parser.paths();
    if (paths.empty()) {
        std::cerr << name << " paths is empty" << std::endl;
        std::exit(1);
    }

    size_t firstCallbacks = observer.notify(parser);
    if (firstCallbacks == 0) {
        std::cerr << name << " notify produced zero callbacks" << std::endl;
        std::exit(1);
    }

    uint64_t totalNs = 0;
    uint64_t minNs = std::numeric_limits<uint64_t>::max();
    uint64_t maxNs = 0;
    for (size_t i = 0; i < rounds; ++i) {
        const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        size_t callbacks = observer.notify(parser);
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        if (callbacks != firstCallbacks) {
            std::cerr << name << " callback count changed: " << callbacks << " != " << firstCallbacks << std::endl;
            std::exit(1);
        }

        uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        totalNs += ns;
        if (ns < minNs) {
            minNs = ns;
        }
        if (ns > maxNs) {
            maxNs = ns;
        }
    }

    const double avgMs = static_cast<double>(totalNs) / static_cast<double>(rounds) / 1000000.0;
    const double minMs = static_cast<double>(minNs) / 1000000.0;
    const double maxMs = static_cast<double>(maxNs) / 1000000.0;

    std::cout << name
              << " bytes=" << configBytes
              << " paths=" << paths.size()
              << " subscriptions=" << (largeSubscriptions ? 172 : 8)
              << " callbacks=" << firstCallbacks
              << " rounds=" << rounds
              << " notify_avg_ms=" << avgMs
              << " notify_min_ms=" << minMs
              << " notify_max_ms=" << maxMs
              << std::endl;
}

} // namespace

int main()
{
    const size_t pluginCount = 1200;
    const size_t rounds = 50;

    const std::string yaml = BuildYaml(pluginCount);
    eular::YamlParser yamlParser;
    eular::ConfigResult yamlRes = yamlParser.loadFromString(yaml);
    if (yamlRes.code() != eular::CONFIG_OK) {
        std::cerr << "yaml load failed: " << yamlRes.message() << std::endl;
        return 1;
    }
    RunNotifyBenchmark<eular::YamlParser, eular::YamlNode>("yaml-small", yamlParser, yaml.size(), rounds, false);
    RunNotifyBenchmark<eular::YamlParser, eular::YamlNode>("yaml-large", yamlParser, yaml.size(), rounds, true);

    const std::string json = BuildJson(pluginCount);
    eular::JsonParser jsonParser;
    eular::ConfigResult jsonRes = jsonParser.loadFromString(json);
    if (jsonRes.code() != eular::CONFIG_OK) {
        std::cerr << "json load failed: " << jsonRes.message() << std::endl;
        return 1;
    }
    RunNotifyBenchmark<eular::JsonParser, eular::JsonNode>("json-small", jsonParser, json.size(), rounds, false);
    RunNotifyBenchmark<eular::JsonParser, eular::JsonNode>("json-large", jsonParser, json.size(), rounds, true);

    return 0;
}
