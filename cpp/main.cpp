#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include "config.hpp"
#include "search.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string env_path = "../.env";
    std::string html_path = "../index.html";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (arg == "--env" && i + 1 < argc) env_path = argv[++i];
        else if (arg == "--html" && i + 1 < argc) html_path = argv[++i];
    }

    Config cfg;
    cfg.load(env_path);
    std::cout << "[init] Config loaded from " << env_path << "\n";

    SearchEngine engine(cfg);
    httplib::Server svr;

    // CORS middleware
    svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "*");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // API: search
    svr.Get("/api/search", [&](const httplib::Request& req, httplib::Response& res) {
        auto q = req.get_param_value("q");
        auto eng = req.get_param_value("engine_name");
        auto region = req.get_param_value("region");
        int num = 10;

        if (q.empty()) {
            res.set_content(R"({"error":"missing q parameter"})", "application/json");
            return;
        }
        if (eng.empty()) eng = "duckduckgo";
        if (region.empty()) region = "kz";

        auto num_str = req.get_param_value("num");
        if (!num_str.empty()) num = std::stoi(num_str);
        if (num < 1) num = 1;
        if (num > 30) num = 30;

        auto result = engine.search(q, eng, num, region);
        res.set_content(result.dump(), "application/json");
    });

    // API: regions
    svr.Get("/api/regions", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"regions":["kz","ru","global"]})", "application/json");
    });

    // API: health
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok","server":"cpp"})", "application/json");
    });

    // Serve index.html
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        std::ifstream f(html_path);
        if (f.is_open()) {
            std::stringstream ss;
            ss << f.rdbuf();
            res.set_content(ss.str(), "text/html");
        } else {
            res.status = 404;
            res.set_content("index.html not found", "text/plain");
        }
    });

    std::cout << "[*] SearchX C++ engine starting on port " << port << "\n";
    std::cout << "[*] Engines: Google(Serper), Yandex(Cloud API)\n";
    svr.listen("0.0.0.0", port);
    return 0;
}
