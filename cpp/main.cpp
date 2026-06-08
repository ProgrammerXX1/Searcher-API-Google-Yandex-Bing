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

        bool with_oa = req.get_param_value("oa") == "true";
        json result;
        try {
            result = engine.search(q, eng, num, region, with_oa);
        } catch (const std::exception& e) {
            result = {{"error", std::string("search exception: ") + e.what()},
                      {"query", q}, {"engine", eng}, {"results", json::array()}};
        } catch (...) {
            result = {{"error", "search unknown exception"},
                      {"query", q}, {"engine", eng}, {"results", json::array()}};
        }
        // replace handler: tolerate any invalid UTF-8 from scraped/3rd-party data
        res.set_content(result.dump(-1, ' ', false, json::error_handler_t::replace),
                        "application/json");
    });

    // API: plagiarism check (POST {text}) — scans text against OpenAlex + Springer
    svr.Post("/api/plagiarism", [&](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("text")) {
            res.set_content(R"({"error":"missing text"})", "application/json");
            return;
        }
        std::string text = body.value("text", "");
        if (text.empty()) {
            res.set_content(R"({"error":"empty text"})", "application/json");
            return;
        }
        auto result = engine.check_plagiarism(text);
        res.set_content(result.dump(-1, ' ', false, json::error_handler_t::replace),
                        "application/json");
    });

    // API: crawl (fetch a public URL through Decodo residential, extract metadata)
    svr.Get("/api/crawl", [&](const httplib::Request& req, httplib::Response& res) {
        auto target = req.get_param_value("url");
        if (target.empty()) {
            res.set_content(R"({"error":"missing url parameter"})", "application/json");
            return;
        }
        // country = residential exit (default "us"); "direct" → no proxy.
        auto country = req.get_param_value("country");
        if (country.empty()) country = "us";
        if (country == "direct") country = "";

        int max_links = 50;
        auto ml = req.get_param_value("max_links");
        if (!ml.empty()) { max_links = std::stoi(ml); if (max_links < 0) max_links = 0; if (max_links > 300) max_links = 300; }

        int max_text = 1000;
        auto mt = req.get_param_value("max_text");
        if (!mt.empty()) { max_text = std::stoi(mt); if (max_text < 0) max_text = 0; }

        json result;
        try {
            result = engine.crawl_metadata(target, country, max_links, max_text);
        } catch (const std::exception& e) {
            result = {{"url", target}, {"error", std::string("crawl exception: ") + e.what()},
                      {"bytes", 0}};
        } catch (...) {
            result = {{"url", target}, {"error", "crawl unknown exception"}, {"bytes", 0}};
        }
        res.set_content(result.dump(-1, ' ', false, json::error_handler_t::replace),
                        "application/json");
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

    // Serve academic.html (separate frontend for OpenAlex/Springer)
    std::string academic_path = html_path;
    {
        auto p = academic_path.rfind("index.html");
        if (p != std::string::npos) academic_path.replace(p, 10, "academic.html");
        else academic_path = "academic.html";
    }
    svr.Get("/academic", [academic_path](const httplib::Request&, httplib::Response& res) {
        std::ifstream f(academic_path);
        if (f.is_open()) {
            std::stringstream ss;
            ss << f.rdbuf();
            res.set_content(ss.str(), "text/html");
        } else {
            res.status = 404;
            res.set_content("academic.html not found", "text/plain");
        }
    });

    // Serve plagiarism.html (text-vs-academic-DB similarity checker)
    std::string plag_path = html_path;
    {
        auto p = plag_path.rfind("index.html");
        if (p != std::string::npos) plag_path.replace(p, 10, "plagiarism.html");
        else plag_path = "plagiarism.html";
    }
    svr.Get("/plagiarism", [plag_path](const httplib::Request&, httplib::Response& res) {
        std::ifstream f(plag_path);
        if (f.is_open()) {
            std::stringstream ss;
            ss << f.rdbuf();
            res.set_content(ss.str(), "text/html");
        } else {
            res.status = 404;
            res.set_content("plagiarism.html not found", "text/plain");
        }
    });

    // Catch unhandled exceptions in httplib worker threads
    svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res,
                                  std::exception_ptr ep) {
        std::string what = "unknown";
        try { if (ep) std::rethrow_exception(ep); }
        catch (const std::exception& e) { what = e.what(); }
        catch (...) {}
        std::cerr << "[!] exception in " << req.method << " " << req.path << ": " << what << "\n";
        res.status = 500;
        res.set_content("{\"error\":\"internal: " + what + "\"}", "application/json");
    });

    std::cout << "[*] SearchX C++ engine starting on port " << port << "\n";
    std::cout << "[*] Engines: Google + Yandex (Decodo residential scrape)\n";
    svr.listen("0.0.0.0", port);
    return 0;
}
