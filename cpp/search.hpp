#pragma once
#include <string>
#include <vector>
#include <regex>
#include <future>
#include <algorithm>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include "config.hpp"
#include "http_client.hpp"

using json = nlohmann::json;

struct SearchResult {
    std::string title, url, domain, snippet;

    json to_json() const {
        return {{"title", title}, {"url", url}, {"domain", domain}, {"snippet", snippet}};
    }
};

// ============ Base64 decode (for Yandex API) ============
static const std::string B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_decode(const std::string& in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[B64[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ============ URL encode ============
static std::string url_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ============ Extract domain from URL ============
static std::string extract_domain(const std::string& url) {
    auto start = url.find("://");
    if (start == std::string::npos) return "";
    start += 3;
    auto end = url.find('/', start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (host.substr(0, 4) == "www.") host = host.substr(4);
    return host;
}

// ============ Simple HTML text extraction ============
static std::string strip_tags(const std::string& html) {
    std::string out;
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out += c;
    }
    return out;
}

class SearchEngine {
    Config& cfg;
    HttpClient http;

public:
    explicit SearchEngine(Config& c) : cfg(c) {}

    // ==================== Google (Serper API) ====================
    std::vector<SearchResult> search_google_serper(const std::string& query, int num,
                                                    const std::string& region) {
        std::string gl = "us", hl = "en";
        if (region == "kz") { gl = "kz"; hl = "ru"; }
        else if (region == "ru") { gl = "ru"; hl = "ru"; }

        json body = {{"q", query}, {"num", num}, {"gl", gl}, {"hl", hl}};
        auto resp = http.post_json(
            "https://google.serper.dev/search",
            body.dump(),
            "X-API-KEY: " + cfg.serper_key,
            10000
        );

        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("organic")) return results;

        for (auto& item : data["organic"]) {
            if ((int)results.size() >= num) break;
            std::string url = item.value("link", "");
            if (url.empty()) continue;
            results.push_back({
                item.value("title", ""),
                url,
                extract_domain(url),
                item.value("snippet", ""),
            });
        }
        return results;
    }

    // ==================== Google (Startpage fallback) ====================
    std::vector<SearchResult> search_google_startpage(const std::string& query, int num) {
        std::string form = "query=" + url_encode(query) + "&cat=web&language=english";
        auto resp = http.post_form(
            "https://www.startpage.com/sp/search",
            form,
            cfg.google_proxy_url(),
            20000
        );

        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        // Parse with regex — Startpage has consistent structure
        std::regex link_re(R"xx(class="wgl-title-link-container[^"]*"[^>]*>\s*<a[^>]*href="(https?://[^"]+)"[^>]*>([^<]+))xx");
        std::regex snippet_re(R"xx(<p class="[^"]*w-gl__description[^"]*">([^<]+))xx");

        auto body = resp.body;
        auto it = std::sregex_iterator(body.begin(), body.end(), link_re);
        auto end = std::sregex_iterator();

        for (; it != end && (int)results.size() < num; ++it) {
            std::string url = (*it)[1].str();
            std::string title = strip_tags((*it)[2].str());
            if (url.find("startpage") != std::string::npos) continue;

            // Find snippet near this position
            std::string snippet;
            auto pos = it->position() + it->length();
            auto sub = body.substr(pos, 1000);
            std::smatch sm;
            if (std::regex_search(sub, sm, snippet_re)) {
                snippet = strip_tags(sm[1].str());
            }

            results.push_back({title, url, extract_domain(url), snippet});
        }
        return results;
    }

    // ==================== Google (combined) ====================
    std::vector<SearchResult> search_google(const std::string& query, int num,
                                             const std::string& region) {
        if (!cfg.serper_key.empty()) {
            auto r = search_google_serper(query, num, region);
            if (!r.empty()) return r;
        }
        return search_google_startpage(query, num);
    }

    // ==================== Yandex (Cloud API) ====================
    std::vector<SearchResult> search_yandex_api(const std::string& query, int num,
                                                 const std::string& region) {
        std::string region_id = "162"; // Almaty
        if (region == "ru") region_id = "225";
        else if (region == "global") region_id = "0";

        json body = {
            {"query", {{"searchType", "SEARCH_TYPE_RU"}, {"queryText", query}, {"page", 0}}},
            {"groupSpec", {{"groupMode", "GROUP_MODE_FLAT"}, {"groupsOnPage", num}, {"docsInGroup", 1}}},
            {"maxPassages", 1},
            {"region", region_id},
            {"l10n", "LOCALIZATION_RU"},
            {"folderId", cfg.yandex_folder_id},
        };

        auto resp = http.post_json(
            "https://searchapi.api.cloud.yandex.net/v2/web/search",
            body.dump(),
            "Authorization: Api-Key " + cfg.yandex_api_key,
            15000
        );

        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("rawData")) return results;

        std::string xml = base64_decode(data["rawData"].get<std::string>());

        pugi::xml_document doc;
        doc.load_string(xml.c_str());

        for (auto group : doc.select_nodes("//group")) {
            if ((int)results.size() >= num) break;
            auto node = group.node();
            auto doc_node = node.child("doc");
            if (!doc_node) continue;

            std::string url = doc_node.child_value("url");
            if (url.empty()) continue;

            std::string title = strip_tags(doc_node.child("title").text().as_string());
            std::string snippet;
            auto passages = doc_node.child("passages");
            if (passages) {
                snippet = strip_tags(passages.child("passage").text().as_string());
            }
            std::string domain = doc_node.child_value("domain");
            if (domain.empty()) domain = extract_domain(url);
            if (domain.substr(0, 4) == "www.") domain = domain.substr(4);

            results.push_back({title, url, domain, snippet});
        }
        return results;
    }

    // ==================== Yandex (scraping fallback) ====================
    std::vector<SearchResult> search_yandex_scrape(const std::string& query, int num) {
        std::string url = "https://yandex.kz/search/?text=" + url_encode(query) + "&numdoc=" + std::to_string(num);
        auto resp = http.get(url, cfg.proxy_url("kz"), 20000);

        std::vector<SearchResult> results;
        if (resp.status != 200 || resp.body.find("verification") != std::string::npos) return results;

        // Extract links from serp-items using regex
        std::regex item_re(R"xx(<li class="serp-item"[^>]*>([\s\S]*?)</li>)xx");
        std::regex h2_re(R"xx(<h2[^>]*>([\s\S]*?)</h2>)xx");
        std::regex href_re(R"xx(href="(https?://(?!.*(?:yandex|clck))[^"]+)")xx");

        auto it = std::sregex_iterator(resp.body.begin(), resp.body.end(), item_re);
        for (; it != std::sregex_iterator() && (int)results.size() < num; ++it) {
            std::string item = (*it)[1].str();

            std::smatch h2m;
            if (!std::regex_search(item, h2m, h2_re)) continue;
            std::string title = strip_tags(h2m[1].str());
            if (title.empty()) continue;

            std::smatch hm;
            if (!std::regex_search(item, hm, href_re)) continue;
            std::string link = hm[1].str();

            results.push_back({title, link, extract_domain(link), ""});
        }
        return results;
    }

    // ==================== Yandex (combined) ====================
    std::vector<SearchResult> search_yandex(const std::string& query, int num,
                                             const std::string& region) {
        if (!cfg.yandex_api_key.empty() && !cfg.yandex_folder_id.empty()) {
            auto r = search_yandex_api(query, num, region);
            if (!r.empty()) return r;
        }
        return search_yandex_scrape(query, num);
    }

    // ==================== DuckDuckGo ====================
    std::vector<SearchResult> search_duckduckgo(const std::string& query, int num,
                                                 const std::string& region) {
        std::string url = "https://html.duckduckgo.com/html/?q=" + url_encode(query);
        auto resp = http.get(url, cfg.proxy_url(region), 20000);

        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        std::regex result_re(R"xx(<a[^>]*class="result__a"[^>]*href="([^"]*)"[^>]*>([^<]+))xx");
        std::regex snippet_re(R"xx(<a[^>]*class="result__snippet"[^>]*>([^<]+))xx");

        auto it = std::sregex_iterator(resp.body.begin(), resp.body.end(), result_re);
        for (; it != std::sregex_iterator() && (int)results.size() < num; ++it) {
            std::string href = (*it)[1].str();
            std::string title = (*it)[2].str();

            // Extract real URL from uddg param
            auto uddg_pos = href.find("uddg=");
            std::string real_url;
            if (uddg_pos != std::string::npos) {
                auto start = uddg_pos + 5;
                auto end = href.find('&', start);
                real_url = href.substr(start, end == std::string::npos ? std::string::npos : end - start);
                // URL decode
                CURL* curl = curl_easy_init();
                if (curl) {
                    int out_len;
                    char* decoded = curl_easy_unescape(curl, real_url.c_str(), real_url.size(), &out_len);
                    if (decoded) { real_url = std::string(decoded, out_len); curl_free(decoded); }
                    curl_easy_cleanup(curl);
                }
            } else if (href.find("http") == 0 && href.find("duckduckgo") == std::string::npos) {
                real_url = href;
            } else {
                continue;
            }

            // Find snippet
            std::string snippet;
            auto pos = it->position();
            auto sub = resp.body.substr(pos, 2000);
            std::smatch sm;
            if (std::regex_search(sub, sm, snippet_re)) {
                snippet = sm[1].str();
            }

            results.push_back({title, real_url, extract_domain(real_url), snippet});
        }
        return results;
    }

    // ==================== Bing ====================
    std::vector<SearchResult> search_bing(const std::string& query, int num,
                                           const std::string& region) {
        std::string url = "https://www.bing.com/search?q=" + url_encode(query) + "&count=" + std::to_string(num);
        auto resp = http.get(url, cfg.proxy_url(region), 20000);

        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        std::regex algo_re(R"xx(<li class="b_algo">([\s\S]*?)</li>)xx");
        std::regex title_re(R"xx(<h2><a[^>]*>([^<]+)</a>)xx");
        std::regex cite_re(R"xx(<cite[^>]*>([^<]+))xx");
        std::regex snippet_re(R"xx(<p[^>]*>([^<]{20,}))xx");

        auto it = std::sregex_iterator(resp.body.begin(), resp.body.end(), algo_re);
        for (; it != std::sregex_iterator() && (int)results.size() < num; ++it) {
            std::string item = (*it)[1].str();

            std::smatch tm;
            if (!std::regex_search(item, tm, title_re)) continue;
            std::string title = tm[1].str();

            std::smatch cm;
            if (!std::regex_search(item, cm, cite_re)) continue;
            std::string cite_url = cm[1].str();
            // Clean cite
            auto sp = cite_url.find(' ');
            if (sp != std::string::npos) cite_url = cite_url.substr(0, sp);
            while (!cite_url.empty() && cite_url.back() == '/') cite_url.pop_back();
            if (cite_url.find("http") != 0) cite_url = "https://" + cite_url;

            std::string snippet;
            std::smatch sm;
            if (std::regex_search(item, sm, snippet_re)) {
                snippet = sm[1].str();
            }

            results.push_back({title, cite_url, extract_domain(cite_url), snippet});
        }
        return results;
    }

    // ==================== All ====================
    std::vector<SearchResult> search_all(const std::string& query, int num,
                                          const std::string& region) {
        // Run all engines in parallel
        auto f_google = std::async(std::launch::async, [&]{ return search_google(query, num, region); });
        auto f_yandex = std::async(std::launch::async, [&]{ return search_yandex(query, num, region); });
        auto f_ddg = std::async(std::launch::async, [&]{ return search_duckduckgo(query, num, region); });
        auto f_bing = std::async(std::launch::async, [&]{ return search_bing(query, num, region); });

        auto google = f_google.get();
        auto yandex = f_yandex.get();
        auto ddg = f_ddg.get();
        auto bing = f_bing.get();

        // Merge + deduplicate
        std::set<std::string> seen;
        std::vector<SearchResult> merged;
        for (auto* src : {&google, &yandex, &ddg, &bing}) {
            for (auto& r : *src) {
                if (seen.insert(r.domain).second) {
                    merged.push_back(r);
                }
            }
        }
        return merged;
    }

    // ==================== Main dispatch ====================
    json search(const std::string& query, const std::string& engine, int num,
                const std::string& region) {
        std::vector<SearchResult> results;

        if (engine == "google") results = search_google(query, num, region);
        else if (engine == "yandex") results = search_yandex(query, num, region);
        else if (engine == "bing") results = search_bing(query, num, region);
        else if (engine == "scholar") results = {}; // TODO
        else if (engine == "all") results = search_all(query, num, region);
        else results = search_duckduckgo(query, num, region);

        json j_results = json::array();
        for (auto& r : results) j_results.push_back(r.to_json());

        return {
            {"error", nullptr},
            {"query", query},
            {"engine", engine},
            {"region", region},
            {"count", results.size()},
            {"results", j_results},
        };
    }
};
