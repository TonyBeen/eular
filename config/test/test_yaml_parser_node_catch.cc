#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include "catch/catch.hpp"

#include <config/yaml.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

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

std::string BasicYaml()
{
    return std::string(
        "apiVersion: 1.1\n"
        "kind: Deployment\n"
        "metadata:\n"
        "  labels:\n"
        "    app: nginx\n"
        "  name: nginx-demo\n"
        "spec:\n"
        "  selector:\n"
        "    app: nginx\n"
        "  volum:\n"
        "    containers:\n"
        "      - image: nginx:1.14\n"
        "        imagePullPolicy: IfNotPresent\n"
        "      - image: websocket:2.10\n"
        "        imagePullPolicy: IfNotPresent\n"
        "log:\n"
        "  level: info\n"
        "  target: stdout\n"
        "  sync: true\n"
        "  int: 10\n"
        "  double: 3.1415\n");
}

std::string BoolYaml()
{
    return std::string(
        "bools:\n"
        "  lower_true: true\n"
        "  upper_true: TRUE\n"
        "  mixed_yes: YeS\n"
        "  lower_false: false\n"
        "  upper_false: FALSE\n"
        "  mixed_no: nO\n"
        "  one: 1\n"
        "  zero: 0\n"
        "text: hello\n"
        "number: 42\n"
        "pi: 3.14\n");
}
} // namespace

TEST_CASE("YamlParser load/getNode/foreach full path", "[yaml][parser][node]")
{
    eular::YamlParser parser;
    std::string yaml = BasicYaml();

    eular::ConfigResult loadFromStringRes = parser.loadFromString(yaml.c_str(), static_cast<uint32_t>(yaml.size()));
    REQUIRE(loadFromStringRes.code() == eular::CONFIG_OK);

    eular::ConfigResult loadFromStdStringRes = parser.loadFromString(yaml);
    REQUIRE(loadFromStdStringRes.code() == eular::CONFIG_OK);

    parser.foreachNode();

    eular::YamlNode root = parser.getNode("$");
    REQUIRE(root.valid());
    REQUIRE(root.isMap());
    REQUIRE(root.type() == eular::YamlNode::NodeType::Map);
    REQUIRE(root.size() >= 5);

    eular::YamlNode appNode = parser.getNode("$spec.selector.app");
    REQUIRE(appNode.valid());
    REQUIRE(appNode.isScalar());
    CHECK(appNode.scalar() == "nginx");

    eular::YamlNode containersNode = parser.getNode("$spec.volum.containers");
    REQUIRE(containersNode.valid());
    REQUIRE(containersNode.isSequence());
    REQUIRE(containersNode.size() == 2);
    CHECK(containersNode.at(0).at("image").scalar() == "nginx:1.14");
    CHECK(containersNode.at(1).at("image").scalar() == "websocket:2.10");
    CHECK_FALSE(containersNode.at(2).valid());

    CHECK_FALSE(root.at("unknown").valid());
    CHECK_FALSE(parser.getNode("$missing.path").valid());

    int logInt = 0;
    REQUIRE(parser.getNode("$log.int").as(&logInt));
    CHECK(logInt == 10);

    double logDouble = 0.0;
    REQUIRE(parser.getNode("$log.double").as(&logDouble));
    CHECK(logDouble == Approx(3.1415));

    bool sync = false;
    REQUIRE(parser.getNode("$log.sync").as(&sync));
    CHECK(sync == true);

    std::string kind;
    REQUIRE(parser.getNode("$kind").as(&kind));
    CHECK(kind == "Deployment");

    CHECK(parser.getNode("$kind").as<int>(777) == 777);

    const std::string sampleYamlPath = TestDataFilePath("syntax_sample.yaml");
    eular::ConfigResult loadFromPathRes1 = parser.load(sampleYamlPath.c_str());
    REQUIRE(loadFromPathRes1.code() == eular::CONFIG_OK);
    eular::ConfigResult loadFromPathRes2 = parser.load(sampleYamlPath);
    REQUIRE(loadFromPathRes2.code() == eular::CONFIG_OK);

    std::string appName;
    REQUIRE(parser.getNode("$app.name").as(&appName));
    CHECK(appName == "downloader");

    parser.reset();
    CHECK_FALSE(parser.getNode("$").valid());
    parser.foreachNode();

    eular::ConfigResult reloadAfterReset = parser.loadFromString(yaml);
    REQUIRE(reloadAfterReset.code() == eular::CONFIG_OK);
    REQUIRE(parser.getNode("$spec.selector.app").scalar() == "nginx");
}

TEST_CASE("YamlParser error branches", "[yaml][parser][error]")
{
    eular::YamlParser parser;

    eular::ConfigResult nullFileRes = parser.load(static_cast<const char *>(nullptr));
    REQUIRE(nullFileRes.code() == eular::CONFIG_INVALID_ARGUMENT);

    const std::string missingPath = TestDataFilePath("not_exists_abcdef.yaml");
    eular::ConfigResult notFoundRes = parser.load(missingPath.c_str());
    REQUIRE(notFoundRes.code() == eular::CONFIG_NOT_FOUND);

    const std::string emptyFilePath = TestDataFilePath("empty.yaml");
    eular::ConfigResult emptyFileRes = parser.load(emptyFilePath.c_str());
    REQUIRE(emptyFileRes.code() == eular::CONFIG_FILE_EMPTY);

    eular::ConfigResult nullYamlRes = parser.loadFromString(static_cast<const char *>(nullptr), 16);
    REQUIRE(nullYamlRes.code() == eular::CONFIG_INVALID_ARGUMENT);

    eular::ConfigResult zeroSizeRes = parser.loadFromString("abc", 0);
    REQUIRE(zeroSizeRes.code() == eular::CONFIG_INVALID_ARGUMENT);

    eular::ConfigResult parseErrorRes = parser.loadFromString("x: [1, 2");
    REQUIRE(parseErrorRes.code() == eular::CONFIG_PARSE_ERROR);

    eular::ConfigResult unsupportedRes = parser.loadFromString("? {x: 1, y: 2}\n: value2\n");
    REQUIRE(unsupportedRes.code() == eular::CONFIG_UNSUPPORTED);
}

TEST_CASE("YamlNode c4 conversion and bool variants", "[yaml][node][convert]")
{
    eular::YamlParser parser;
    eular::ConfigResult res = parser.loadFromString(BoolYaml());
    REQUIRE(res.code() == eular::CONFIG_OK);

    CHECK(parser.getNode("$bools.lower_true").as<bool>(false) == true);
    CHECK(parser.getNode("$bools.upper_true").as<bool>(false) == true);
    CHECK(parser.getNode("$bools.mixed_yes").as<bool>(false) == true);

    CHECK(parser.getNode("$bools.lower_false").as<bool>(true) == false);
    CHECK(parser.getNode("$bools.upper_false").as<bool>(true) == false);
    CHECK(parser.getNode("$bools.mixed_no").as<bool>(true) == false);

    CHECK(parser.getNode("$bools.one").as<bool>(false) == true);
    CHECK(parser.getNode("$bools.zero").as<bool>(true) == false);

    int number = 0;
    REQUIRE(parser.getNode("$number").as(&number));
    CHECK(number == 42);

    double pi = 0.0;
    REQUIRE(parser.getNode("$pi").as(&pi));
    CHECK(pi == Approx(3.14));

    std::string text;
    REQUIRE(parser.getNode("$text").as(&text));
    CHECK(text == "hello");

    int bad = 0;
    CHECK_FALSE(parser.getNode("$text").as(&bad));
}

TEST_CASE("YamlParser concurrent load and read", "[yaml][parser][thread]")
{
    std::vector<std::string> docs;
    docs.push_back("counter: 1\nflag: true\nitems: [10, 20]\n");
    docs.push_back("counter: 2\nflag: NO\nitems: [30, 40]\n");
    docs.push_back("counter: 3\nflag: YeS\nitems: [50, 60]\n");

    eular::YamlParser parser;
    REQUIRE(parser.loadFromString(docs[0]).code() == eular::CONFIG_OK);

    std::atomic<int> errors(0);
    std::atomic<bool> stop(false);

    std::thread writer([&parser, &docs, &errors, &stop]() {
        for (int i = 0; i < 2000; ++i) {
            eular::ConfigResult r = parser.loadFromString(docs[static_cast<size_t>(i) % docs.size()]);
            if (r.code() != eular::CONFIG_OK) {
                errors.fetch_add(1);
            }
        }
        stop.store(true);
    });

    auto readerTask = [&parser, &errors, &stop]() {
        while (!stop.load()) {
            eular::YamlNode root = parser.getNode("$");
            if (!root.valid() || !root.isMap()) {
                errors.fetch_add(1);
                continue;
            }

            eular::YamlNode counterNode = root.at("counter");
            int counter = 0;
            if (!counterNode.as(&counter)) {
                errors.fetch_add(1);
                continue;
            }
            if (counter < 1 || counter > 3) {
                errors.fetch_add(1);
            }

            bool flag = false;
            if (!root.at("flag").as(&flag)) {
                errors.fetch_add(1);
            }

            eular::YamlNode items = root.at("items");
            if (!items.valid() || !items.isSequence() || items.size() != 2) {
                errors.fetch_add(1);
                continue;
            }

            int firstItem = -1;
            if (!items.at(0).as(&firstItem)) {
                errors.fetch_add(1);
                continue;
            }

            if (counter == 1 && firstItem != 10) {
                errors.fetch_add(1);
            }
            if (counter == 2 && firstItem != 30) {
                errors.fetch_add(1);
            }
            if (counter == 3 && firstItem != 50) {
                errors.fetch_add(1);
            }
        }
    };

    std::thread reader1(readerTask);
    std::thread reader2(readerTask);
    std::thread reader3(readerTask);
    std::thread reader4(readerTask);

    writer.join();
    reader1.join();
    reader2.join();
    reader3.join();
    reader4.join();

    REQUIRE(errors.load() == 0);
}
