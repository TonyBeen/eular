#include <config/xml.h>

#include <fstream>
#include <iostream>
#include <string>

namespace {
std::string buildXmlText()
{
    return std::string(
        "<root>"
        "  <server>"
        "    <host>192.168.3.10</host>"
        "    <port>9000</port>"
        "    <enabled>yes</enabled>"
        "    <ratio>3.5</ratio>"
        "    <bad_int>not_number</bad_int>"
        "  </server>"
        "  <logging>"
        "    <level>debug</level>"
        "    <console>true</console>"
        "  </logging>"
        "</root>");
}

bool writeXmlFile(const std::string &path, const std::string &xmlText)
{
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << xmlText;
    return out.good();
}
} // namespace

int main(int argc, char **argv)
{
    eular::XmlConfig xml;

    std::string xmlText = buildXmlText();
    if (!xml.parse(xmlText)) {
        std::cerr << "parse(xml text) failed" << std::endl;
        return 1;
    }

    std::cout << "from parse():" << std::endl;
    std::cout << "server.host=" << xml.lookup<std::string>("root.server.host", "") << std::endl;
    std::cout << "server.port=" << xml.lookup<int>("root.server.port", 0) << std::endl;
    std::cout << "server.enabled=" << xml.lookup<bool>("root.server.enabled", false) << std::endl;
    std::cout << "server.ratio=" << xml.lookup<double>("root.server.ratio", 0.0) << std::endl;
    std::cout << "bad_int fallback=" << xml.lookup<int>("root.server.bad_int", 888) << std::endl;

    std::string xmlPath;
    if (argc > 1) {
        xmlPath = argv[1];
    } else {
        xmlPath = "example_xml_complex.xml";
        if (!writeXmlFile(xmlPath, xmlText)) {
            std::cerr << "failed to create xml file: " << xmlPath << std::endl;
            return 2;
        }
    }

    if (!xml.loadFile(xmlPath)) {
        std::cerr << "loadFile failed: " << xmlPath << std::endl;
        return 3;
    }

    std::cout << "from loadFile():" << std::endl;
    std::cout << "logging.level=" << xml.lookup<std::string>("root.logging.level", "info") << std::endl;
    std::cout << "logging.console=" << xml.lookup<bool>("root.logging.console", false) << std::endl;

    xml.foreach();
    return 0;
}
