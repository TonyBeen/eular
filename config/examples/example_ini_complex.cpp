#include <config/ini.h>

#include <fstream>
#include <iostream>
#include <string>

namespace {
bool writeIniFile(const std::string &path)
{
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out
        << "app_name = config_demo\n"
        << "global_timeout = 1200\n"
        << "\n"
        << "[database]\n"
        << "host = 127.0.0.1\n"
        << "port = 5432\n"
        << "user = admin\n"
        << "\n"
        << "[service]\n"
        << "retries = 5\n"
        << "ratio = 2.75\n"
        << "tls = yes\n";

    return out.good();
}
} // namespace

int main(int argc, char **argv)
{
    std::string iniPath;
    if (argc > 1) {
        iniPath = argv[1];
    } else {
        iniPath = "example_ini_complex.ini";
        if (!writeIniFile(iniPath)) {
            std::cerr << "failed to create ini file: " << iniPath << std::endl;
            return 1;
        }
    }

    eular::IniConfig ini;
    if (!ini.parser(iniPath)) {
        std::cerr << "failed to parse ini: " << iniPath << std::endl;
        return 2;
    }

    std::string appName = ini.lookup<std::string>("app_name", "unknown");
    int timeout = ini.lookup<int>("global_timeout", 0);
    std::string dbHost = ini.lookup<std::string>("database.host", "127.0.0.1");
    int dbPort = ini.lookup<int>("database.port", 3306);
    bool serviceTls = ini.lookup<bool>("service.tls", false);
    double serviceRatio = ini.lookup<double>("service.ratio", 1.0);

    std::cout << "app_name=" << appName << ", timeout=" << timeout << std::endl;
    std::cout << "database=" << dbHost << ":" << dbPort << std::endl;
    std::cout << "service tls=" << serviceTls << ", ratio=" << serviceRatio << std::endl;

    ini["database.timeout_ms"] = "2500";
    ini["service.tls"] = "no";
    ini["dynamic.cache_size"] = "1024";

    int dbTimeout = ini.lookup<int>("database.timeout_ms", -1);
    bool tlsAfterUpdate = ini.lookup<bool>("service.tls", true);
    int invalidCastFallback = ini.lookup<int>("database.host", 777);

    std::cout << "database.timeout_ms=" << dbTimeout << std::endl;
    std::cout << "service.tls(after update)=" << tlsAfterUpdate << std::endl;
    std::cout << "invalid cast fallback=" << invalidCastFallback << std::endl;

    ini.del("database.user");

    std::string outPath = iniPath + ".out.ini";
    if (!ini.keep(outPath)) {
        std::cerr << "failed to write ini: " << outPath << std::endl;
        return 3;
    }

    std::cout << "saved updated ini to: " << outPath << std::endl;
    return 0;
}
