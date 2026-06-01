#pragma once
#include <string>
#include <vector>
#include <regex>
#include <future>
#include <algorithm>
#include <set>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstdlib>
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

    // ==================== Google (Decodo residential scrape of Startpage) ====================
    std::vector<SearchResult> search_google_startpage(const std::string& query, int num) {
        std::string form = "query=" + url_encode(query) + "&cat=web&language=english";

        // Parse with regex — title link is <a class="result-title result-link" href=...>
        // followed (after an optional inline <style>) by the title inside <h2>.
        std::regex link_re(R"xx(class="result-title result-link[^"]*"[^>]*href="(https?://[^"]+)"[\s\S]{0,400}?<h2[^>]*>(?:<[^>]+>)*([^<]+))xx");
        std::regex snippet_re(R"xx(class="description[^"]*">([\s\S]*?)</p>)xx");

        // Decodo residential, country-targeted + browser-fingerprint headers + HTTP/2 holds
        // ~95% on Startpage with no volume cliff. Remaining misses are isolated challenge
        // pages; each retry rotates both the exit country and the IP to clear them.
        static const char* countries[] = {"us", "gb", "de"};
        const int max_attempts = 3;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(400 + (std::rand() % 600)));
            }

            auto resp = http.post_form(
                "https://www.startpage.com/sp/search",
                form,
                cfg.google_scrape_proxy_url(countries[attempt % 3]),
                20000,
                /*fresh_connection=*/true,
                /*browser=*/true
            );
            if (resp.status != 200) continue;

            std::vector<SearchResult> results;
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

            if (!results.empty()) return results;  // success — stop retrying
        }
        return {};  // all attempts returned a challenge/empty page
    }

    // ==================== Google (combined) ====================
    std::vector<SearchResult> search_google(const std::string& query, int num,
                                             const std::string& /*region*/) {
        // Google results now come entirely from Decodo (residential scrape of Startpage),
        // no external SERP API.
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

    // ==================== AI Summary (YandexGPT) ====================
    std::string ai_summarize(const std::string& query, const std::vector<SearchResult>& results) {
        if (cfg.yandex_api_key.empty() || cfg.yandex_folder_id.empty() || results.empty())
            return "";

        // Build context from top results
        std::string context;
        for (int i = 0; i < std::min((int)results.size(), 5); i++) {
            context += std::to_string(i + 1) + ". " + results[i].title;
            if (!results[i].snippet.empty())
                context += " — " + results[i].snippet;
            context += "\n";
        }

        json body = {
            {"modelUri", "gpt://" + cfg.yandex_folder_id + "/yandexgpt-lite/latest"},
            {"completionOptions", {{"stream", false}, {"temperature", 0.3}, {"maxTokens", 400}}},
            {"messages", {
                {{"role", "system"}, {"text",
                    "You are a search assistant. Based on the search results provided, "
                    "give a concise and helpful answer to the user's query. "
                    "Answer in the same language as the query. 3-5 sentences max. "
                    "Do not mention that you are looking at search results."}},
                {{"role", "user"}, {"text", "Query: " + query + "\n\nSearch results:\n" + context}},
            }},
        };

        auto resp = http.post_json(
            "https://llm.api.cloud.yandex.net/foundationModels/v1/completion",
            body.dump(),
            "Authorization: Api-Key " + cfg.yandex_api_key,
            15000
        );

        if (resp.status != 200) return "";

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded()) return "";

        try {
            return data["result"]["alternatives"][0]["message"]["text"].get<std::string>();
        } catch (...) {
            return "";
        }
    }

    // ==================== Chat (follow-up) ====================
    json chat(const json& messages) {
        using clock = std::chrono::steady_clock;

        if (cfg.yandex_api_key.empty() || cfg.yandex_folder_id.empty())
            return {{"error", "no API key"}, {"reply", ""}};

        json gpt_messages = json::array();
        gpt_messages.push_back({
            {"role", "system"},
            {"text", "You are a helpful search assistant. Answer concisely in the same language the user writes. "
                     "If the user asks a follow-up, use the conversation context to answer."}
        });

        for (auto& msg : messages) {
            gpt_messages.push_back({
                {"role", msg.value("role", "user")},
                {"text", msg.value("content", "")}
            });
        }

        json body = {
            {"modelUri", "gpt://" + cfg.yandex_folder_id + "/yandexgpt-lite/latest"},
            {"completionOptions", {{"stream", false}, {"temperature", 0.4}, {"maxTokens", 500}}},
            {"messages", gpt_messages},
        };

        auto t0 = clock::now();
        auto resp = http.post_json(
            "https://llm.api.cloud.yandex.net/foundationModels/v1/completion",
            body.dump(),
            "Authorization: Api-Key " + cfg.yandex_api_key,
            15000
        );
        auto t1 = clock::now();
        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (resp.status != 200)
            return {{"error", "API error"}, {"reply", ""}, {"ms", ms}};

        auto data = json::parse(resp.body, nullptr, false);
        std::string reply;
        try {
            reply = data["result"]["alternatives"][0]["message"]["text"].get<std::string>();
        } catch (...) {}

        return {{"error", nullptr}, {"reply", reply}, {"ms", ms}};
    }

    // ==================== Main dispatch ====================
    json search(const std::string& query, const std::string& engine, int num,
                const std::string& region, bool with_ai = false) {
        using clock = std::chrono::steady_clock;

        auto t0 = clock::now();
        std::vector<SearchResult> results;

        if (engine == "google") results = search_google(query, num, region);
        else if (engine == "yandex") results = search_yandex(query, num, region);
        else if (engine == "bing") results = search_bing(query, num, region);
        else if (engine == "all") results = search_all(query, num, region);
        else results = search_duckduckgo(query, num, region);

        auto t1 = clock::now();
        long search_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        json j_results = json::array();
        for (auto& r : results) j_results.push_back(r.to_json());

        json response = {
            {"error", nullptr},
            {"query", query},
            {"engine", engine},
            {"region", region},
            {"count", results.size()},
            {"results", j_results},
            {"search_ms", search_ms},
        };

        if (with_ai && !results.empty()) {
            auto t2 = clock::now();
            response["ai_summary"] = ai_summarize(query, results);
            auto t3 = clock::now();
            response["ai_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
        }

        return response;
    }
};
