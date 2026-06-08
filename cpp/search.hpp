#pragma once
#include <string>
#include <vector>
#include <regex>
#include <future>
#include <algorithm>
#include <set>
#include <map>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include "config.hpp"
#include "http_client.hpp"

using json = nlohmann::json;

struct SearchResult {
    std::string title, url, domain, snippet;
    // Optional academic metadata (populated by scholarly engines only)
    std::string doi, authors, year;
    long cited_by = -1;
    // Full article text (CORE / CyberLeninka) — plagiarism body-matching, never serialized.
    std::string full_text;
    // Unpaywall-resolved Open Access full-text link (enrichment via ?oa=true).
    std::string oa_url;

    json to_json() const {
        json j = {{"title", title}, {"url", url}, {"domain", domain}, {"snippet", snippet}};
        if (!doi.empty())     j["doi"] = doi;
        if (!authors.empty())  j["authors"] = authors;
        if (!year.empty())     j["year"] = year;
        if (cited_by >= 0)     j["cited_by"] = cited_by;
        if (!oa_url.empty())   j["oa_url"] = oa_url;
        return j;
    }
};

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

// ============ Case-insensitive find (shared helper) ============
static size_t ifind(const std::string& s, const std::string& needle, size_t from = 0) {
    for (size_t i = from; i + needle.size() <= s.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower((unsigned char)s[i+j]) != std::tolower((unsigned char)needle[j])) {
                match = false; break;
            }
        }
        if (match) return i;
    }
    return std::string::npos;
}

// ============ Crawler metadata helpers ============
// Read a single attribute value from a tag string, e.g. attr_value(tag, "content").
// Case-insensitive on the attribute name; matches both "..." and '...' quoting.
static std::string attr_value(const std::string& tag, const std::string& name) {
    // Case-insensitive find of the attribute name
    size_t pos = ifind(tag, name);
    if (pos == std::string::npos) return "";
    pos += name.size();
    // Skip whitespace around '='
    while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '\t')) ++pos;
    if (pos >= tag.size() || tag[pos] != '=') return "";
    ++pos;
    while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '\t')) ++pos;
    if (pos >= tag.size()) return "";
    // Detect quote character
    char q = tag[pos];
    if (q != '"' && q != '\'') return "";
    ++pos;
    auto end = tag.find(q, pos);
    if (end == std::string::npos) return "";
    return tag.substr(pos, end - pos);
}

// Drop <script>/<style> blocks before text extraction so their bodies don't pollute
// the readable text snippet.  Uses find-based loop instead of regex (regex with
// [\s\S]*? causes stack overflow on large HTML pages).
static std::string strip_scripts(std::string html) {
    // Remove all <script ...>...</script> blocks (case-insensitive)
    for (const char* open_tag : {"<script", "<style"}) {
        std::string close_tag = std::string("</") + (open_tag + 1) + ">"; // </script> or </style>
        std::string out;
        size_t pos = 0;
        while (pos < html.size()) {
            size_t start = ifind(html, open_tag, pos);
            if (start == std::string::npos) {
                out.append(html, pos, html.size() - pos);
                break;
            }
            out.append(html, pos, start - pos);
            out += ' ';
            size_t end = ifind(html, close_tag, start + std::strlen(open_tag));
            if (end == std::string::npos) break; // unclosed tag — drop the rest
            pos = end + close_tag.size();
        }
        html = std::move(out);
    }
    return html;
}

// Collapse runs of whitespace into single spaces and trim — readable one-line text.
static std::string collapse_ws(const std::string& s) {
    std::string out;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!out.empty()) sp = true;
        } else {
            if (sp) out += ' ';
            sp = false;
            out += c;
        }
    }
    return out;
}

// Resolve a possibly-relative href against the page URL (handles //, /, and absolute).
static std::string resolve_url(const std::string& base, const std::string& href) {
    if (href.empty()) return "";
    if (href.compare(0, 4, "http") == 0) return href;
    if (href.compare(0, 2, "//") == 0) {
        auto scheme_end = base.find("://");
        std::string scheme = (scheme_end != std::string::npos)
            ? base.substr(0, scheme_end) : "https";
        return scheme + ":" + href;
    }
    // origin = scheme://host
    auto s = base.find("://");
    if (s == std::string::npos) return "";
    auto host_end = base.find('/', s + 3);
    std::string origin = base.substr(0, host_end == std::string::npos ? base.size() : host_end);
    if (href[0] == '/') return origin + href;
    return origin + "/" + href;
}

// ============ Safe JSON accessors (tolerate null / wrong type) ============
// nlohmann's value() throws when a key exists but holds null; these never throw.
static std::string jstr(const json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : "";
}
static long jint(const json& j, const char* key, long def = 0) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    return (it != j.end() && it->is_number_integer()) ? it->get<long>() : def;
}

// Truncate to ~max bytes on a UTF-8 character boundary (never split a multibyte
// codepoint — invalid UTF-8 makes json::dump throw). Shared by the scholarly engines.
static std::string trunc_utf8(std::string s, size_t max = 600) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
    return s.substr(0, cut) + "...";
}

// First string from a field that may be a plain string or an array of strings.
static std::string jfirst(const json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    if (it == j.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_array() && !it->empty() && it->front().is_string())
        return it->front().get<std::string>();
    return "";
}

// OpenAIRE wraps scalar values as {"$": value}. Unwrap to a string (handles raw
// string / number too).
static std::string oa_text(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_object()) {
        auto it = j.find("$");
        if (it != j.end()) {
            if (it->is_string()) return it->get<std::string>();
            if (it->is_number_integer()) return std::to_string(it->get<long>());
            if (it->is_number()) return std::to_string(it->get<double>());
        }
    }
    return "";
}

// ============ OpenAlex abstract reconstruction ============
// OpenAlex returns abstracts as an inverted index {word: [positions]} (licensing
// workaround). Rebuild the plain-text abstract by placing each word at its position.
static std::string reconstruct_abstract(const json& inv) {
    if (!inv.is_object() || inv.empty()) return "";
    std::map<int, std::string> ordered;
    for (auto it = inv.begin(); it != inv.end(); ++it) {
        if (!it.value().is_array()) continue;
        for (auto& p : it.value()) {
            if (p.is_number_integer()) ordered[p.get<int>()] = it.key();
        }
    }
    std::string out;
    for (auto& [pos, word] : ordered) {
        if (!out.empty()) out += ' ';
        out += word;
    }
    if (out.size() > 600) {
        // Back off to a UTF-8 character boundary so we never split a multibyte
        // codepoint (would produce invalid UTF-8 and make json::dump throw).
        size_t cut = 600;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;
        out = out.substr(0, cut) + "...";
    }
    return out;
}

// ============ Plagiarism helpers (language-agnostic, UTF-8 safe) ============
// Word tokenizer: keeps multibyte (Cyrillic / Kazakh / etc.) byte sequences inside
// words, lowercases ASCII, splits on ASCII punctuation & whitespace. Works for ru/en/kz.
static std::vector<std::string> plag_tokenize(const std::string& s) {
    std::vector<std::string> toks;
    std::string cur;
    for (unsigned char c : s) {
        if (c >= 0x80) {                 // UTF-8 continuation/lead byte → part of a word
            cur += static_cast<char>(c);
        } else if (std::isalnum(c)) {
            cur += static_cast<char>(std::tolower(c));
        } else if (!cur.empty()) {
            toks.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

// Word n-grams (shingles). Verbatim copy → near-total shingle overlap; paraphrase → less.
static std::set<std::string> plag_shingles(const std::vector<std::string>& toks, int k) {
    std::set<std::string> out;
    if ((int)toks.size() < k) {
        std::string j;
        for (auto& t : toks) { if (!j.empty()) j += ' '; j += t; }
        if (!j.empty()) out.insert(j);
        return out;
    }
    for (int i = 0; i + k <= (int)toks.size(); ++i) {
        std::string g;
        for (int x = 0; x < k; ++x) { if (x) g += ' '; g += toks[i + x]; }
        out.insert(g);
    }
    return out;
}

// Containment of A in B: |A∩B| / |A|. Asymmetric on purpose — we ask "how much of the
// user's sentence appears in this source", not symmetric Jaccard (source is far longer).
static double plag_containment(const std::set<std::string>& a, const std::set<std::string>& b) {
    if (a.empty()) return 0.0;
    size_t inter = 0;
    for (auto& x : a) if (b.count(x)) ++inter;
    return static_cast<double>(inter) / a.size();
}

// Split text into sentences on . ! ? and newlines; trims and keeps only chunks with
// enough words to be a meaningful match (short fragments produce noise).
static std::vector<std::string> plag_sentences(const std::string& text, size_t min_words = 6) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]{
        size_t b = cur.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) { cur.clear(); return; }
        size_t e = cur.find_last_not_of(" \t\r\n");
        std::string s = cur.substr(b, e - b + 1);
        cur.clear();
        if (plag_tokenize(s).size() >= min_words) out.push_back(s);
    };
    for (char c : text) {
        cur += c;
        if (c == '.' || c == '!' || c == '?' || c == '\n') flush();
    }
    flush();
    return out;
}

class SearchEngine {
    Config& cfg;
    HttpClient http;

    // --- Yandex session state (per-region cookie jar + expiry) ---
    struct YandexSession {
        CookieJar cookies;
        std::chrono::steady_clock::time_point expires_at{};
        bool valid() const {
            return !cookies.empty() &&
                   std::chrono::steady_clock::now() < expires_at;
        }
    };
    std::unordered_map<std::string, YandexSession> yx_sessions_;
    std::mutex yx_mtx_;

    // Detect Yandex JS-challenge / captcha page (no organic results, needs JS execution).
    static bool is_yandex_challenge(const std::string& body) {
        return body.find("SmartCaptcha") != std::string::npos
            || body.find("showcaptcha")  != std::string::npos
            || body.find("captcha")      != std::string::npos
            || body.find("tmgrdfrend")   != std::string::npos
            || body.find("verification") != std::string::npos
            || body.find("CheckboxCaptcha") != std::string::npos;
    }

    // Region → {yandex host, proxy country, lr code}.
    // yandex.kz has the weakest anti-bot; yandex.ru/com aggressively serve SmartCaptcha.
    // All regions go through yandex.kz with the lr= parameter to select result locality:
    //   lr=162 → Almaty (KZ),  lr=213 → Moscow (RU),  lr=84 → USA.
    struct YxRegion { std::string host, country, lr; };
    static YxRegion yx_region(const std::string& region) {
        if (region == "ru")     return {"yandex.kz", "kz", "213"};
        if (region == "global") return {"yandex.kz", "kz", "84"};
        return {"yandex.kz", "kz", "162"};   // default: Almaty
    }

    // Warm a Yandex session: visit the homepage to collect cookies (yandexuid, i, yp).
    // Must be called from a thread that already holds yx_mtx_ or before concurrent use.
    void warm_yandex_session(const std::string& region) {
        auto r = yx_region(region);
        auto& sess = yx_sessions_[region];

        std::string home = "https://" + r.host + "/";
        // First touch — fresh residential IP to get a clean session.
        http.get_with_jar(home, cfg.residential_proxy_url(r.country),
                          sess.cookies, 15000, /*browser=*/true,
                          /*fresh=*/true);

        sess.expires_at = std::chrono::steady_clock::now()
                        + std::chrono::minutes(30);
    }

    // Get (or create) a valid Yandex session for this region.
    YandexSession& get_yandex_session(const std::string& region) {
        std::lock_guard<std::mutex> lk(yx_mtx_);
        auto& sess = yx_sessions_[region];
        if (!sess.valid()) warm_yandex_session(region);
        return sess;
    }

    // Parse Yandex SERP HTML into SearchResult vector.
    // 2024-2026 layout: React-based components inside <li class="serp-item ...">:
    //   Title:   <span class="OrganicTitleContentSpan" ...>text</span>
    //   URL:     <a class="... OrganicTitle-Link ..." href="URL">
    //   Snippet: <span class="OrganicTextContentSpan" ...>text</span>
    static std::vector<SearchResult> parse_yandex_html(const std::string& body, int num) {
        // Find-based parsing — no regex on full SERP HTML (avoids stack overflow).
        std::vector<SearchResult> results;
        const std::string item_open = "<li class=\"serp-item";
        const std::string item_close = "</li>";

        size_t pos = 0;
        while ((int)results.size() < num) {
            // Find next serp-item <li>
            size_t li_start = body.find(item_open, pos);
            if (li_start == std::string::npos) break;
            size_t li_end = body.find(item_close, li_start + item_open.size());
            if (li_end == std::string::npos) break;
            li_end += item_close.size();

            std::string item = body.substr(li_start, li_end - li_start);
            pos = li_end;

            // Title — find OrganicTitleContentSpan, then extract to </span>
            size_t t = item.find("OrganicTitleContentSpan");
            if (t == std::string::npos) continue;
            size_t t_gt = item.find('>', t);
            if (t_gt == std::string::npos) continue;
            size_t t_end = item.find("</span>", t_gt + 1);
            if (t_end == std::string::npos) continue;
            std::string title = strip_tags(item.substr(t_gt + 1, t_end - t_gt - 1));
            if (title.empty()) continue;

            // URL — find OrganicTitle-Link, then extract href="..."
            size_t l = item.find("OrganicTitle-Link");
            if (l == std::string::npos) continue;
            size_t href_pos = item.find("href=\"", l);
            if (href_pos == std::string::npos) continue;
            href_pos += 6;
            size_t href_end = item.find('"', href_pos);
            if (href_end == std::string::npos) continue;
            std::string link = item.substr(href_pos, href_end - href_pos);
            // Must start with http
            if (link.compare(0, 4, "http") != 0) continue;
            // Skip yandex/clck internal redirects
            if (link.find("yandex") != std::string::npos ||
                link.find("clck") != std::string::npos) continue;

            // Snippet — find OrganicTextContentSpan, extract to </span>
            std::string snippet;
            size_t s = item.find("OrganicTextContentSpan");
            if (s != std::string::npos) {
                size_t s_gt = item.find('>', s);
                if (s_gt != std::string::npos) {
                    size_t s_end = item.find("</span>", s_gt + 1);
                    if (s_end != std::string::npos)
                        snippet = collapse_ws(strip_tags(item.substr(s_gt + 1, s_end - s_gt - 1)));
                }
            }

            results.push_back({title, link, extract_domain(link), snippet});
        }
        return results;
    }

    // Fallback: Decodo SERP Scraping API (handles JS challenges server-side).
    std::vector<SearchResult> search_yandex_serp_api(const std::string& query, int num,
                                                      const std::string& region) {
        if (!cfg.has_serp_api()) return {};
        auto r = yx_region(region);

        // Build the full Yandex URL (gives us control over lr, numdoc).
        std::string yx_url = "https://" + r.host + "/search/?text="
            + url_encode(query) + "&numdoc=" + std::to_string(num);
        if (!r.lr.empty()) yx_url += "&lr=" + r.lr;

        json payload = {
            {"target", "yandex"},
            {"url",    yx_url},
            {"parse",  false}
        };
        auto resp = http.post_json(
            "https://scraper-api.decodo.com/v2/scrape",
            payload.dump(), cfg.serp_auth(), 30000);

        if (resp.status != 200) return {};
        // SERP API returns raw Yandex HTML — parse with the same regex pipeline.
        return parse_yandex_html(resp.body, num);
    }

public:
    explicit SearchEngine(Config& c) : cfg(c) {}

    // ==================== Google (Decodo residential scrape of Startpage) ====================
    std::vector<SearchResult> search_google_startpage(const std::string& query, int num) {
        std::string form = "query=" + url_encode(query) + "&cat=web&language=english";

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
                cfg.residential_proxy_url(countries[attempt % 3]),
                20000,
                /*fresh_connection=*/true,
                /*browser=*/true
            );
            if (resp.status != 200) continue;

            // Find-based parsing — no regex on full Startpage HTML.
            std::vector<SearchResult> results;
            const auto& body = resp.body;
            const std::string link_marker = "class=\"result-title result-link";
            const std::string desc_marker = "class=\"description";

            size_t pos = 0;
            while ((int)results.size() < num) {
                // Find the result-title link
                size_t m = body.find(link_marker, pos);
                if (m == std::string::npos) break;

                // Extract href="..."
                size_t href_pos = body.find("href=\"", m);
                if (href_pos == std::string::npos || href_pos > m + 300) { pos = m + link_marker.size(); continue; }
                href_pos += 6;
                size_t href_end = body.find('"', href_pos);
                if (href_end == std::string::npos) break;
                std::string url = body.substr(href_pos, href_end - href_pos);

                // Find <h2> after the <a> tag — title is inside
                size_t h2_pos = body.find("<h2", href_end);
                if (h2_pos == std::string::npos || h2_pos > href_end + 400) { pos = href_end; continue; }
                size_t h2_gt = body.find('>', h2_pos);
                if (h2_gt == std::string::npos) break;
                // Find closing </h2> or end of first text run
                size_t h2_end = body.find("</h2>", h2_gt + 1);
                std::string title;
                if (h2_end != std::string::npos)
                    title = strip_tags(body.substr(h2_gt + 1, h2_end - h2_gt - 1));

                pos = (h2_end != std::string::npos) ? h2_end + 5 : href_end + 1;

                if (url.compare(0, 4, "http") != 0) continue;
                if (url.find("startpage") != std::string::npos) continue;
                if (title.empty()) continue;

                // Find snippet: class="description" nearby (within 1000 chars)
                std::string snippet;
                size_t d = body.find(desc_marker, pos);
                if (d != std::string::npos && d < pos + 1000) {
                    size_t d_gt = body.find('>', d);
                    if (d_gt != std::string::npos) {
                        size_t d_end = body.find("</p>", d_gt + 1);
                        if (d_end != std::string::npos)
                            snippet = strip_tags(body.substr(d_gt + 1, d_end - d_gt - 1));
                    }
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

    // ==================== Yandex (anti-bot hardened scrape) ====================
    // Five-layer anti-detection pipeline:
    //   1. TLS/HTTP2 fingerprint  — curl-impersonate (when linked) or Chrome headers
    //   2. Cookie session         — persistent yandexuid/i/yp from homepage warm-up
    //   3. Behavioral suggest     — pre-query to suggest.yandex → natural navigation chain
    //   4. Human timing           — 800-2500ms typing delay + 300-800ms suggest→search gap
    //   5. SERP API fallback      — Decodo scraper API for remaining JS-challenge pages
    //
    // NOTE: For full TLS/HTTP2 mimicry (steps 1-2 of the anti-detection plan), link against
    // libcurl-impersonate instead of system libcurl. Set cmake -DUSE_CURL_IMPERSONATE=ON
    // and the library calls curl_easy_impersonate(h, "chrome131", 0) automatically. Without
    // it, regular libcurl + browser headers still passes ~80% of Yandex's checks.
    std::vector<SearchResult> search_yandex(const std::string& query, int num,
                                             const std::string& region) {
        auto r = yx_region(region);
        auto& sess = get_yandex_session(region);
        std::string proxy = cfg.residential_proxy_url(r.country);
        std::string home_url = "https://" + r.host + "/";

        // --- Behavioral layer: suggest pre-query (typing simulation) ---
        // Real users type in the search box → Yandex fires suggest requests → then /search.
        // This creates a natural navigation chain that lowers the bot-score.
        {
            // Typing delay (800-2500ms) — simulates human input speed.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(800 + (std::rand() % 1700)));

            // Send a suggest request with a partial query (first half of words).
            std::string partial = query;
            auto sp = partial.rfind(' ', partial.size() / 2);
            if (sp != std::string::npos && sp > 0) partial = partial.substr(0, sp);

            std::string tld = r.host.substr(r.host.rfind('.') + 1); // kz, ru, com
            std::string suggest_url = "https://suggest.yandex." + tld
                + "/suggest-ya.cgi?part=" + url_encode(partial)
                + "&uil=ru&v=4&sn=5";
            if (!r.lr.empty()) suggest_url += "&lr=" + r.lr;

            // Referer = homepage (user typed from the main page).
            http.get_with_jar(suggest_url, proxy, sess.cookies, 5000,
                              /*browser=*/true, /*fresh=*/false, home_url);

            // Gap before search (300-800ms) — user picks a suggestion or presses Enter.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(300 + (std::rand() % 500)));
        }

        // --- Main search request ---
        std::string search_url = "https://" + r.host + "/search/?text="
            + url_encode(query) + "&numdoc=" + std::to_string(num);
        if (!r.lr.empty()) search_url += "&lr=" + r.lr;

        const int max_attempts = 3;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(400 + (std::rand() % 600)));
                // Rotate IP on retry but keep cookies (session stickiness).
            }

            // Reuse TCP/session (same residential IP) — NOT fresh_connection on first try.
            // Fresh only on retries to rotate IP after a challenge page.
            auto resp = http.get_with_jar(search_url, proxy, sess.cookies, 20000,
                                          /*browser=*/true,
                                          /*fresh=*/attempt > 0,
                                          home_url);

            if (resp.status != 200) continue;
            if (is_yandex_challenge(resp.body)) continue;

            auto results = parse_yandex_html(resp.body, num);
            if (!results.empty()) return results;
        }

        // --- Fallback: Decodo SERP API (outsources JS execution + captcha solving) ---
        auto fallback = search_yandex_serp_api(query, num, region);
        if (!fallback.empty()) return fallback;

        return {};
    }

    // ==================== DuckDuckGo ====================
    // DDG Lite (html.duckduckgo.com) blocks datacenter IPs with "bots use DuckDuckGo too".
    // Route through Decodo residential proxy to appear as a real browser.
    std::vector<SearchResult> search_duckduckgo(const std::string& query, int num,
                                                 const std::string& region) {
        std::string url = "https://html.duckduckgo.com/html/?q=" + url_encode(query);

        // Try residential proxy first, fall back to mobile proxy.
        static const char* countries[] = {"us", "gb", "de"};
        const int max_attempts = 3;

        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            if (attempt > 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(400 + (std::rand() % 600)));

            auto resp = http.get(url, cfg.residential_proxy_url(countries[attempt % 3]),
                                 20000, /*browser=*/true, /*fresh_connection=*/true);

            if (resp.status != 200) continue;
            // DDG bot-detection page contains "anomaly-modal" — skip and retry.
            if (resp.body.find("anomaly-modal") != std::string::npos) continue;

            // Find-based parsing — no regex on full DDG HTML.
            std::vector<SearchResult> results;
            const std::string result_marker = "class=\"result__a\"";
            const std::string snippet_marker = "class=\"result__snippet\"";
            const auto& rbody = resp.body;

            size_t fpos = 0;
            while ((int)results.size() < num) {
                // Find next result__a link
                size_t a_pos = rbody.find(result_marker, fpos);
                if (a_pos == std::string::npos) break;

                // Find the enclosing <a tag start (search backwards)
                size_t a_open = rbody.rfind("<a", a_pos);
                if (a_open == std::string::npos) { fpos = a_pos + result_marker.size(); continue; }

                // Extract href="..."
                size_t href_pos = rbody.find("href=\"", a_open);
                if (href_pos == std::string::npos || href_pos > a_pos + result_marker.size() + 100) { fpos = a_pos + result_marker.size(); continue; }
                href_pos += 6;
                size_t href_end = rbody.find('"', href_pos);
                if (href_end == std::string::npos) break;
                std::string href = rbody.substr(href_pos, href_end - href_pos);

                // Extract title text (after the closing > of the <a> tag)
                size_t a_gt = rbody.find('>', a_pos);
                if (a_gt == std::string::npos) break;
                size_t title_end = rbody.find('<', a_gt + 1);
                std::string title;
                if (title_end != std::string::npos)
                    title = rbody.substr(a_gt + 1, title_end - a_gt - 1);

                fpos = (title_end != std::string::npos) ? title_end : a_gt + 1;

                // Extract real URL from uddg param
                auto uddg_pos = href.find("uddg=");
                std::string real_url;
                if (uddg_pos != std::string::npos) {
                    auto start = uddg_pos + 5;
                    auto end = href.find('&', start);
                    real_url = href.substr(start, end == std::string::npos ? std::string::npos : end - start);
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

                // Find snippet nearby (within 2000 chars)
                std::string snippet;
                size_t s_pos = rbody.find(snippet_marker, fpos);
                if (s_pos != std::string::npos && s_pos < fpos + 2000) {
                    size_t s_gt = rbody.find('>', s_pos);
                    if (s_gt != std::string::npos) {
                        size_t s_end = rbody.find('<', s_gt + 1);
                        if (s_end != std::string::npos)
                            snippet = rbody.substr(s_gt + 1, s_end - s_gt - 1);
                    }
                }

                results.push_back({title, real_url, extract_domain(real_url), snippet});
            }
            if (!results.empty()) return results;
        }
        return {};
    }

    // ==================== Bing ====================
    // Bing's HTML SERP is now fully JS-rendered (no b_algo in raw HTML). Use the RSS
    // feed instead — it returns structured XML with title, link, and description, works
    // without JS execution, and doesn't require a proxy.
    std::vector<SearchResult> search_bing(const std::string& query, int num,
                                           const std::string& /*region*/) {
        std::string url = "https://www.bing.com/search?format=rss&q="
            + url_encode(query) + "&count=" + std::to_string(num);

        auto resp = http.get(url, "", 15000);  // RSS works direct, no proxy needed
        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        // Find-based RSS <item> parsing — no regex on XML body.
        const auto& rbody = resp.body;
        const std::string item_open = "<item>";
        const std::string item_close = "</item>";

        auto extract_xml = [](const std::string& s, const std::string& tag) -> std::string {
            std::string open = "<" + tag + ">";
            std::string close = "</" + tag + ">";
            size_t p = s.find(open);
            if (p == std::string::npos) return "";
            p += open.size();
            size_t e = s.find(close, p);
            if (e == std::string::npos) return "";
            return s.substr(p, e - p);
        };

        size_t bpos = 0;
        while ((int)results.size() < num) {
            size_t i_start = rbody.find(item_open, bpos);
            if (i_start == std::string::npos) break;
            i_start += item_open.size();
            size_t i_end = rbody.find(item_close, i_start);
            if (i_end == std::string::npos) break;
            bpos = i_end + item_close.size();

            std::string item = rbody.substr(i_start, i_end - i_start);
            std::string title = strip_tags(extract_xml(item, "title"));
            std::string link = extract_xml(item, "link");
            std::string snippet = strip_tags(extract_xml(item, "description"));
            if (title.empty() || link.empty()) continue;
            if (link.find("bing.com") != std::string::npos) continue;

            results.push_back({title, link, extract_domain(link), snippet});
        }
        return results;
    }

    // ==================== OpenAlex (free scholarly API, no key) ====================
    std::vector<SearchResult> search_openalex(const std::string& query, int num) {
        std::string url = "https://api.openalex.org/works?search=" + url_encode(query)
            + "&per-page=" + std::to_string(num)
            + "&select=title,doi,publication_year,cited_by_count,authorships,"
              "abstract_inverted_index,primary_location";
        if (!cfg.openalex_mailto.empty())
            url += "&mailto=" + url_encode(cfg.openalex_mailto);

        auto resp = http.get(url, "", 15000); // direct call, no proxy
        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("results")) return results;

        for (auto& w : data["results"]) {
            if ((int)results.size() >= num) break;
            SearchResult r;
            r.title = jstr(w, "title");
            if (r.title.empty()) continue;

            r.doi = jstr(w, "doi");                            // full URL form
            // Landing page → DOI → empty
            auto pl = w.find("primary_location");
            if (pl != w.end()) r.url = jstr(*pl, "landing_page_url");
            if (r.url.empty()) r.url = r.doi;

            long yr = jint(w, "publication_year", -1);
            if (yr > 0) r.year = std::to_string(yr);
            r.cited_by = jint(w, "cited_by_count", 0);

            // Authors (first 3)
            auto au = w.find("authorships");
            if (au != w.end() && au->is_array()) {
                int n = 0;
                for (auto& a : *au) {
                    if (n >= 3) { r.authors += " et al."; break; }
                    std::string name = a.is_object() && a.contains("author")
                        ? jstr(a["author"], "display_name") : "";
                    if (name.empty()) continue;
                    if (!r.authors.empty()) r.authors += ", ";
                    r.authors += name;
                    n++;
                }
            }

            auto ab = w.find("abstract_inverted_index");
            if (ab != w.end()) r.snippet = reconstruct_abstract(*ab);

            r.domain = extract_domain(r.url);
            if (r.domain.empty()) r.domain = "openalex.org";
            results.push_back(std::move(r));
        }
        return results;
    }

    // ==================== Springer Nature (Meta API v2, free key) ====================
    std::vector<SearchResult> search_springer(const std::string& query, int num) {
        std::vector<SearchResult> results;
        if (cfg.springer_api_key.empty()) return results; // no key → skip silently

        std::string url = "https://api.springernature.com/meta/v2/json?q=" + url_encode(query)
            + "&p=" + std::to_string(num)
            + "&api_key=" + cfg.springer_api_key;

        auto resp = http.get(url, "", 15000); // direct call, no proxy
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("records")) return results;

        for (auto& rec : data["records"]) {
            if ((int)results.size() >= num) break;
            SearchResult r;
            r.title = jstr(rec, "title");
            if (r.title.empty()) continue;

            r.doi = jstr(rec, "doi");
            // url field is an array of {format,platform,value}
            auto u = rec.find("url");
            if (u != rec.end() && u->is_array() && !u->empty())
                r.url = jstr((*u)[0], "value");
            if (r.url.empty() && !r.doi.empty()) r.url = "https://doi.org/" + r.doi;

            std::string pubdate = jstr(rec, "publicationDate");
            if (pubdate.size() >= 4) r.year = pubdate.substr(0, 4);

            // Authors (creators[].creator)
            auto cr = rec.find("creators");
            if (cr != rec.end() && cr->is_array()) {
                int n = 0;
                for (auto& c : *cr) {
                    if (n >= 3) { r.authors += " et al."; break; }
                    std::string name = jstr(c, "creator");
                    if (name.empty()) continue;
                    if (!r.authors.empty()) r.authors += ", ";
                    r.authors += name;
                    n++;
                }
            }

            r.snippet = jstr(rec, "abstract");
            std::string journal = jstr(rec, "publicationName");
            r.domain = journal.empty() ? "springer.com" : journal;
            results.push_back(std::move(r));
        }
        return results;
    }

    // ==================== CORE (full-text Open Access, API v3) ====================
    std::vector<SearchResult> search_core(const std::string& query, int num) {
        std::vector<SearchResult> results;
        if (cfg.core_api_key.empty()) return results; // no key → skip silently

        json body = {{"q", query}, {"limit", num}};
        auto resp = http.post_json("https://api.core.ac.uk/v3/search/works",
                                   body.dump(),
                                   "Authorization: Bearer " + cfg.core_api_key,
                                   20000);
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("results")) return results;

        for (auto& w : data["results"]) {
            if ((int)results.size() >= num) break;
            SearchResult r;
            r.title = jstr(w, "title");
            if (r.title.empty()) continue;

            r.doi = jstr(w, "doi");
            r.url = jstr(w, "downloadUrl");
            if (r.url.empty()) {
                auto sf = w.find("sourceFulltextUrls");
                if (sf != w.end() && sf->is_array() && !sf->empty() && (*sf)[0].is_string())
                    r.url = (*sf)[0].get<std::string>();
            }
            if (r.url.empty() && !r.doi.empty()) r.url = "https://doi.org/" + r.doi;

            long yr = jint(w, "yearPublished", -1);
            if (yr > 0) r.year = std::to_string(yr);
            r.cited_by = jint(w, "citationCount", 0);

            auto au = w.find("authors");
            if (au != w.end() && au->is_array()) {
                int n = 0;
                for (auto& a : *au) {
                    if (n >= 3) { r.authors += " et al."; break; }
                    std::string name = jstr(a, "name");
                    if (name.empty()) continue;
                    if (!r.authors.empty()) r.authors += ", ";
                    r.authors += name;
                    n++;
                }
            }

            // Abstract → display snippet (UTF-8-safe truncation). Full text kept separately
            // for plagiarism body-matching (not serialized to the client).
            std::string ab = jstr(w, "abstract");
            if (ab.size() > 600) {
                size_t cut = 600;
                while (cut > 0 && (static_cast<unsigned char>(ab[cut]) & 0xC0) == 0x80) cut--;
                ab = ab.substr(0, cut) + "...";
            }
            r.snippet = ab;
            r.full_text = jstr(w, "fullText");

            r.domain = extract_domain(r.url);
            if (r.domain.empty()) r.domain = "core.ac.uk";
            results.push_back(std::move(r));
        }
        return results;
    }

    // ==================== Semantic Scholar (Academic Graph API, free key) ====================
    std::vector<SearchResult> search_semanticscholar(const std::string& query, int num) {
        std::string url = "https://api.semanticscholar.org/graph/v1/paper/search?query="
            + url_encode(query) + "&limit=" + std::to_string(num)
            + "&fields=title,abstract,year,citationCount,authors,externalIds,openAccessPdf,url";

        std::vector<std::string> headers;
        if (!cfg.s2_api_key.empty()) headers.push_back("x-api-key: " + cfg.s2_api_key);

        auto resp = http.get_headers(url, headers, 20000); // direct call; key avoids 429
        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("data")) return results;

        for (auto& w : data["data"]) {
            if ((int)results.size() >= num) break;
            SearchResult r;
            r.title = jstr(w, "title");
            if (r.title.empty()) continue;

            auto ext = w.find("externalIds");
            std::string bare = (ext != w.end() && ext->is_object()) ? jstr(*ext, "DOI") : "";
            if (!bare.empty()) r.doi = "https://doi.org/" + bare;  // full-URL form (as OpenAlex)

            r.url = jstr(w, "url");
            auto oa = w.find("openAccessPdf");
            if (r.url.empty() && oa != w.end() && oa->is_object()) r.url = jstr(*oa, "url");
            if (r.url.empty() && !bare.empty()) r.url = r.doi;

            long yr = jint(w, "year", -1);
            if (yr > 0) r.year = std::to_string(yr);
            r.cited_by = jint(w, "citationCount", 0);

            auto au = w.find("authors");
            if (au != w.end() && au->is_array()) {
                int n = 0;
                for (auto& a : *au) {
                    if (n >= 3) { r.authors += " et al."; break; }
                    std::string name = jstr(a, "name");
                    if (name.empty()) continue;
                    if (!r.authors.empty()) r.authors += ", ";
                    r.authors += name;
                    n++;
                }
            }

            r.snippet = trunc_utf8(jstr(w, "abstract"));
            r.domain = extract_domain(r.url);
            if (r.domain.empty()) r.domain = "semanticscholar.org";
            results.push_back(std::move(r));
        }
        return results;
    }

    // ==================== DOAJ (Directory of Open Access Journals, free, no key) ====================
    std::vector<SearchResult> search_doaj(const std::string& query, int num) {
        // Query goes in the URL path; pageSize caps results.
        std::string url = "https://doaj.org/api/v2/search/articles/" + url_encode(query)
            + "?pageSize=" + std::to_string(num);

        auto resp = http.get(url, "", 15000); // direct call, no proxy
        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("results")) return results;

        for (auto& it : data["results"]) {
            if ((int)results.size() >= num) break;
            auto bj = it.find("bibjson");
            if (bj == it.end() || !bj->is_object()) continue;
            const json& b = *bj;

            SearchResult r;
            r.title = jstr(b, "title");
            if (r.title.empty()) continue;

            r.year = jstr(b, "year"); // DOAJ stores year as a string

            auto ids = b.find("identifier");
            if (ids != b.end() && ids->is_array())
                for (auto& id : *ids)
                    if (jstr(id, "type") == "doi") { r.doi = "https://doi.org/" + jstr(id, "id"); break; }

            // Prefer the fulltext link; otherwise take the first available link.
            auto lk = b.find("link");
            if (lk != b.end() && lk->is_array())
                for (auto& l : *lk) {
                    std::string u = jstr(l, "url");
                    if (u.empty()) continue;
                    r.url = u;
                    if (jstr(l, "type") == "fulltext") break;
                }
            if (r.url.empty() && !r.doi.empty()) r.url = r.doi;

            auto au = b.find("author");
            if (au != b.end() && au->is_array()) {
                int n = 0;
                for (auto& a : *au) {
                    if (n >= 3) { r.authors += " et al."; break; }
                    std::string name = jstr(a, "name");
                    if (name.empty()) continue;
                    if (!r.authors.empty()) r.authors += ", ";
                    r.authors += name;
                    n++;
                }
            }

            r.snippet = trunc_utf8(jstr(b, "abstract"));

            auto jr = b.find("journal");
            std::string jt = (jr != b.end()) ? jstr(*jr, "title") : "";
            r.domain = !jt.empty() ? jt : extract_domain(r.url);
            if (r.domain.empty()) r.domain = "doaj.org";
            results.push_back(std::move(r));
        }
        return results;
    }

    // ==================== DataCite (DOIs for datasets / theses / preprints, free) ====================
    std::vector<SearchResult> search_datacite(const std::string& query, int num) {
        std::string url = "https://api.datacite.org/dois?query=" + url_encode(query)
            + "&page%5Bsize%5D=" + std::to_string(num); // page[size] must be URL-encoded

        auto resp = http.get(url, "", 15000); // direct call, no proxy
        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("data")) return results;

        for (auto& w : data["data"]) {
            if ((int)results.size() >= num) break;
            auto at = w.find("attributes");
            if (at == w.end() || !at->is_object()) continue;
            const json& a = *at;

            SearchResult r;
            auto ti = a.find("titles");
            if (ti != a.end() && ti->is_array() && !ti->empty()) r.title = jstr(ti->front(), "title");
            if (r.title.empty()) continue;

            std::string bare = jstr(a, "doi");
            if (!bare.empty()) r.doi = "https://doi.org/" + bare;
            r.url = jstr(a, "url");
            if (r.url.empty() && !bare.empty()) r.url = r.doi;

            long yr = jint(a, "publicationYear", -1);
            if (yr > 0) r.year = std::to_string(yr);

            auto cr = a.find("creators");
            if (cr != a.end() && cr->is_array()) {
                int n = 0;
                for (auto& c : *cr) {
                    if (n >= 3) { r.authors += " et al."; break; }
                    std::string name = jstr(c, "name");
                    if (name.empty()) continue;
                    if (!r.authors.empty()) r.authors += ", ";
                    r.authors += name;
                    n++;
                }
            }

            auto de = a.find("descriptions");
            if (de != a.end() && de->is_array() && !de->empty())
                r.snippet = trunc_utf8(jstr(de->front(), "description"));

            r.domain = extract_domain(r.url);
            if (r.domain.empty()) r.domain = "datacite.org";
            results.push_back(std::move(r));
        }
        return results;
    }

    // ==================== OpenAIRE (EU OA aggregator, free, no key) ====================
    // Response is deeply nested: response.results.result[].metadata.oaf:entity.oaf:result,
    // with scalars wrapped as {"$": value} and fields that may be object-or-array.
    std::vector<SearchResult> search_openaire(const std::string& query, int num) {
        std::string url = "https://api.openaire.eu/search/publications?keywords="
            + url_encode(query) + "&size=" + std::to_string(num) + "&format=json";

        auto resp = http.get(url, "", 20000); // direct call, no proxy
        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.is_object()) return results;
        auto rsp = data.find("response");
        if (rsp == data.end()) return results;
        auto rsl = rsp->find("results");
        if (rsl == rsp->end() || !rsl->is_object()) return results;
        auto rst = rsl->find("result");
        if (rst == rsl->end()) return results;

        auto process = [&](const json& res) {
            auto md = res.find("metadata");
            if (md == res.end()) return;
            auto ent = md->find("oaf:entity");
            if (ent == md->end()) return;
            auto rr = ent->find("oaf:result");
            if (rr == ent->end()) return;
            const json& o = *rr;

            SearchResult r;
            auto ti = o.find("title");
            if (ti != o.end()) {
                if (ti->is_array()) { for (auto& t : *ti) { r.title = oa_text(t); if (!r.title.empty()) break; } }
                else r.title = oa_text(*ti);
            }
            if (r.title.empty()) return;

            auto pick_doi = [&](const json& p) {
                if (jstr(p, "@classid") == "doi") {
                    std::string d = oa_text(p);
                    if (!d.empty()) r.doi = "https://doi.org/" + d;
                }
            };
            auto pid = o.find("pid");
            if (pid != o.end()) {
                if (pid->is_array()) for (auto& p : *pid) pick_doi(p);
                else pick_doi(*pid);
            }
            r.url = r.doi;

            auto da = o.find("dateofacceptance");
            if (da != o.end()) { std::string d = oa_text(*da); if (d.size() >= 4) r.year = d.substr(0, 4); }

            auto me = o.find("measure");
            if (me != o.end() && me->is_array())
                for (auto& m : *me)
                    if (jstr(m, "@id") == "citationCount") {
                        std::string sc = jstr(m, "@score");
                        if (!sc.empty()) { try { r.cited_by = std::stol(sc); } catch (...) {} }
                    }

            auto creator_name = [](const json& c) -> std::string {
                std::string nm = jstr(c, "@name"), sn = jstr(c, "@surname");
                if (!nm.empty() || !sn.empty()) {
                    std::string full = nm;
                    if (!sn.empty()) { if (!full.empty()) full += " "; full += sn; }
                    return full;
                }
                return oa_text(c);
            };
            auto cr = o.find("creator");
            if (cr != o.end()) {
                if (cr->is_array()) {
                    int n = 0;
                    for (auto& c : *cr) {
                        if (n >= 3) { r.authors += " et al."; break; }
                        std::string f = creator_name(c);
                        if (f.empty()) continue;
                        if (!r.authors.empty()) r.authors += ", ";
                        r.authors += f;
                        n++;
                    }
                } else r.authors = creator_name(*cr);
            }

            auto de = o.find("description");
            if (de != o.end()) {
                std::string ab = de->is_array() ? (de->empty() ? "" : oa_text(de->front())) : oa_text(*de);
                r.snippet = trunc_utf8(ab);
            }

            r.domain = extract_domain(r.url);
            if (r.domain.empty()) r.domain = "openaire.eu";
            results.push_back(std::move(r));
        };

        if (rst->is_array()) { for (auto& res : *rst) { if ((int)results.size() >= num) break; process(res); } }
        else if (rst->is_object()) process(*rst);
        return results;
    }

    // ==================== BASE (Bielefeld Academic Search Engine) ====================
    // NOTE: BASE only answers from a server IP whitelisted at base-search.net (otherwise
    // "Forbidden"). Disabled unless BASE_ENABLED is set. Parser follows BASE's documented
    // Solr-style dc* schema; verify field names once your IP is whitelisted.
    std::vector<SearchResult> search_base(const std::string& query, int num) {
        std::vector<SearchResult> results;
        if (!cfg.base_enabled) return results; // requires whitelisted IP — off by default

        std::string url = "https://www.base-search.net/cgi-bin/BaseHttpSearchInterface.fcgi"
            "?func=PerformSearch&query=" + url_encode(query)
            + "&format=json&hits=" + std::to_string(num);

        auto resp = http.get(url, "", 20000);
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded()) return results;
        auto rp = data.find("response");
        if (rp == data.end() || !rp->is_object()) return results;
        auto dc = rp->find("docs");
        if (dc == rp->end() || !dc->is_array()) return results;

        for (auto& d : *dc) {
            if ((int)results.size() >= num) break;
            SearchResult r;
            r.title = jfirst(d, "dctitle");
            if (r.title.empty()) continue;

            std::string bare = jfirst(d, "dcdoi");
            if (!bare.empty()) r.doi = "https://doi.org/" + bare;
            r.url = jfirst(d, "dclink");
            if (r.url.empty() && !r.doi.empty()) r.url = r.doi;
            r.year = jfirst(d, "dcyear");

            auto cr = d.find("dccreator");
            if (cr != d.end() && cr->is_array()) {
                int n = 0;
                for (auto& c : *cr) {
                    if (!c.is_string()) continue;
                    if (n >= 3) { r.authors += " et al."; break; }
                    if (!r.authors.empty()) r.authors += ", ";
                    r.authors += c.get<std::string>();
                    n++;
                }
            } else r.authors = jfirst(d, "dccreator");

            r.snippet = trunc_utf8(jfirst(d, "dcdescription"));
            r.domain = extract_domain(r.url);
            if (r.domain.empty()) r.domain = "base-search.net";
            results.push_back(std::move(r));
        }
        return results;
    }

    // ==================== CyberLeninka (Russian OA scientific library, full-text) ====================
    // POST search API. Returns Russian articles with annotation (abstract) and `ocr` (full
    // body text — captured into full_text for plagiarism matching, like CORE). Fields carry
    // <b> highlight tags → stripped.
    std::vector<SearchResult> search_cyberleninka(const std::string& query, int num) {
        json body = {{"mode", "articles"}, {"q", query}, {"size", num}, {"from", 0}};
        auto resp = http.post_json("https://cyberleninka.ru/api/search", body.dump(), "", 15000);
        std::vector<SearchResult> results;
        if (resp.status != 200) return results;

        auto data = json::parse(resp.body, nullptr, false);
        if (data.is_discarded() || !data.contains("articles")) return results;

        for (auto& a : data["articles"]) {
            if ((int)results.size() >= num) break;
            SearchResult r;
            r.title = strip_tags(jstr(a, "name"));
            if (r.title.empty()) continue;

            std::string link = jstr(a, "link");
            if (!link.empty()) r.url = (link[0] == '/') ? "https://cyberleninka.ru" + link : link;

            long yr = jint(a, "year", -1);
            if (yr > 0) r.year = std::to_string(yr);

            auto au = a.find("authors");
            if (au != a.end() && au->is_array()) {
                int n = 0;
                for (auto& x : *au) {
                    if (!x.is_string()) continue;
                    if (n >= 3) { r.authors += " и др."; break; }
                    if (!r.authors.empty()) r.authors += ", ";
                    r.authors += x.get<std::string>();
                    n++;
                }
            }

            r.snippet = trunc_utf8(strip_tags(jstr(a, "annotation")));

            // Full body text from ocr[] — kept for plagiarism body-matching, not serialized.
            auto ocr = a.find("ocr");
            if (ocr != a.end() && ocr->is_array()) {
                std::string ft;
                for (auto& p : *ocr) if (p.is_string()) { if (!ft.empty()) ft += ' '; ft += p.get<std::string>(); }
                r.full_text = strip_tags(ft);
            }

            std::string journal = jstr(a, "journal");
            r.domain = journal.empty() ? "cyberleninka.ru" : journal;
            results.push_back(std::move(r));
        }
        return results;
    }

    // ==================== Unpaywall (OA full-text resolver by DOI) ====================
    // Not a search engine — its /v2/search is broken (500). Used as an enrichment layer:
    // given a DOI, return the best Open Access full-text link (PDF preferred).
    std::string unpaywall_oa(const std::string& doi) {
        if (cfg.unpaywall_email.empty() || doi.empty()) return "";
        std::string bare = doi;
        auto p = bare.find("doi.org/");
        if (p != std::string::npos) bare = bare.substr(p + 8); // strip https://doi.org/
        if (bare.empty()) return "";

        std::string url = "https://api.unpaywall.org/v2/" + bare
            + "?email=" + url_encode(cfg.unpaywall_email);
        auto resp = http.get(url, "", 12000); // direct call, no proxy
        if (resp.status != 200) return "";

        auto d = json::parse(resp.body, nullptr, false);
        if (d.is_discarded() || !d.is_object()) return "";
        auto loc = d.find("best_oa_location");
        if (loc == d.end() || !loc->is_object()) return "";
        std::string u = jstr(*loc, "url_for_pdf");
        if (u.empty()) u = jstr(*loc, "url");
        return u;
    }

    // Resolve OA full-text links for all results that carry a DOI, in parallel.
    void enrich_unpaywall(std::vector<SearchResult>& rs) {
        if (cfg.unpaywall_email.empty()) return;
        std::vector<std::future<std::string>> futs(rs.size());
        for (size_t i = 0; i < rs.size(); ++i)
            if (!rs[i].doi.empty())
                futs[i] = std::async(std::launch::async,
                    [this, doi = rs[i].doi] { return unpaywall_oa(doi); });
        for (size_t i = 0; i < rs.size(); ++i)
            if (futs[i].valid()) rs[i].oa_url = futs[i].get();
    }

    // ==================== Plagiarism check (OpenAlex + Springer + CORE) ====================
    // Splits the input into sentences, searches each against the scholarly engines, and
    // scores n-gram containment of the sentence in each candidate. OpenAlex/Springer expose
    // only abstracts; CORE exposes FULL article text, so CORE catches body-level copying that
    // the abstract-only engines miss.
    json check_plagiarism(const std::string& text, int max_chunks = 30) {
        auto sentences = plag_sentences(text);
        if ((int)sentences.size() > max_chunks)
            sentences.resize(max_chunks);

        struct Cand { const char* eng; std::string title, url, doi, year; std::set<std::string> sh; };

        // Springer (500/day) and CORE (5 req/10s) are the rate-limited engines → ONE combined
        // query each for the whole text, reused as shared candidate pools for every sentence.
        auto all_toks = plag_tokenize(text);
        std::string sq;
        for (size_t i = 0; i < all_toks.size() && i < 50; ++i) { if (i) sq += ' '; sq += all_toks[i]; }

        std::vector<Cand> sp_pool;
        for (auto& r : search_springer(sq, 25))
            sp_pool.push_back({"springer", r.title, r.url, r.doi, r.year,
                               plag_shingles(plag_tokenize(r.title + " " + r.snippet), 3)});

        // CORE: shingle the FULL text when present (body-level matching), else the abstract.
        std::vector<Cand> core_pool;
        for (auto& r : search_core(sq, 20)) {
            const std::string& bodytext = r.full_text.empty() ? r.snippet : r.full_text;
            core_pool.push_back({"core", r.title, r.url, r.doi, r.year,
                                 plag_shingles(plag_tokenize(r.title + " " + bodytext), 3)});
        }

        long oa_requests = 0;
        double matched = 0, total = 0;
        json j_chunks = json::array();
        for (auto& sent : sentences) {
            auto toks = plag_tokenize(sent);
            auto sh = plag_shingles(toks, 3);

            // OpenAlex is effectively unlimited (~100k/day) → query per sentence for accurate
            // retrieval (a combined query buries the true source and yields false negatives).
            std::string q = sent;
            if (toks.size() > 32) { q.clear(); for (int i = 0; i < 32; ++i) { if (i) q += ' '; q += toks[i]; } }
            auto oa = search_openalex(q, 5);
            ++oa_requests;

            double best = 0.0;
            const Cand* bestC = nullptr;
            // local copies of this sentence's OpenAlex candidates
            std::vector<Cand> oa_cands;
            for (auto& r : oa)
                oa_cands.push_back({"openalex", r.title, r.url, r.doi, r.year,
                                    plag_shingles(plag_tokenize(r.title + " " + r.snippet), 3)});
            for (auto* pool : {&oa_cands, &sp_pool, &core_pool}) {
                for (auto& c : *pool) {
                    double sim = plag_containment(sh, c.sh);
                    if (sim > best) { best = sim; bestC = &c; }
                }
            }

            json cj = {{"text", sent}, {"score", best}, {"tokens", (long)toks.size()}};
            if (best >= 0.15 && bestC) {
                cj["match"] = {
                    {"engine", bestC->eng}, {"title", bestC->title},
                    {"url", bestC->url}, {"doi", bestC->doi}, {"year", bestC->year},
                };
            }
            total += toks.size();
            if (best >= 0.25) matched += toks.size() * best;
            j_chunks.push_back(cj);
        }
        double overall = total > 0 ? matched / total : 0.0;

        return {
            {"error", nullptr},
            {"overall", overall},                 // 0..1 token-weighted similarity
            {"checked", (long)sentences.size()},
            {"requests_openalex", oa_requests},   // one per sentence (engine is ~unlimited)
            {"requests_springer", 1},             // single combined query (scarce: 500/day)
            {"requests_core", 1},                 // single combined query (scarce: 5 req/10s)
            {"chunks", j_chunks},
        };
    }

    // ==================== Controllable crawler (metadata extraction) ====================
    // Fetch one public URL through a Decodo residential exit and pull out page metadata.
    // This is plain page retrieval + HTML parsing — it does NOT defeat anti-bot/CAPTCHA
    // challenges. Pages behind active bot protection will simply return their challenge
    // page or a non-200, surfaced honestly in the result.
    //   country: residential exit country code (e.g. "us","ru","kz"); "" → direct, no proxy.
    json crawl_metadata(const std::string& target_url, const std::string& country = "us",
                        int max_links = 50, int max_text = 1000) {
        json out = {{"url", target_url}};
        if (target_url.compare(0, 4, "http") != 0) {
            out["error"] = "url must start with http(s)://";
            return out;
        }

        std::string proxy = country.empty() ? "" : cfg.residential_proxy_url(country);
        auto resp = http.get(target_url, proxy, 20000, /*browser=*/true);

        out["status"]      = resp.status;
        out["bytes"]       = (long)resp.body.size();
        out["proxy_country"] = country.empty() ? "direct" : country;
        if (resp.status != 200 || resp.body.empty()) {
            out["error"] = resp.status == 0 ? "fetch failed (network/proxy)" : "non-200 status";
            return out;
        }

        const std::string& html = resp.body;

        // --- Extract metadata with plain string search (no regex on full HTML) ---
        // std::regex + [\s\S]*? on large pages causes stack overflow.
        auto find_tag_content = [](const std::string& s, const std::string& open_tag,
                                    const std::string& close_tag) -> std::string {
            auto pos = s.find(open_tag);
            if (pos == std::string::npos) return "";
            pos += open_tag.size();
            // skip to end of opening tag
            auto gt = s.find('>', pos);
            if (gt == std::string::npos) return "";
            auto start = gt + 1;
            auto end = s.find(close_tag, start);
            if (end == std::string::npos) return "";
            return s.substr(start, end - start);
        };

        // --- <title> ---
        std::string title = collapse_ws(strip_tags(find_tag_content(html, "<title", "</title>")));
        if (title.empty()) title = collapse_ws(strip_tags(find_tag_content(html, "<TITLE", "</TITLE>")));

        // --- <meta> tags (scan first 50 KB only — meta tags are always in <head>) ---
        std::string description, keywords, canonical;
        json og = json::object();
        {
            std::string head = html.substr(0, std::min(html.size(), (size_t)50000));
            std::regex meta_re(R"xx(<meta\b[^>]*>)xx", std::regex::icase);
            auto begin = std::sregex_iterator(head.begin(), head.end(), meta_re);
            for (auto it = begin; it != std::sregex_iterator(); ++it) {
                std::string tag = it->str();
                std::string name = attr_value(tag, "name");
                std::string prop = attr_value(tag, "property");
                std::string content = attr_value(tag, "content");
                if (content.empty()) continue;
                std::string lname = name;
                std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
                if (lname == "description") description = collapse_ws(content);
                else if (lname == "keywords") keywords = collapse_ws(content);
                std::string lprop = prop;
                std::transform(lprop.begin(), lprop.end(), lprop.begin(), ::tolower);
                if (lprop.compare(0, 3, "og:") == 0)
                    og[lprop.substr(3)] = collapse_ws(content);
            }
        }

        // --- <link rel="canonical"> (scan head only) ---
        {
            std::string head = html.substr(0, std::min(html.size(), (size_t)50000));
            std::regex link_re(R"xx(<link\b[^>]*>)xx", std::regex::icase);
            auto begin = std::sregex_iterator(head.begin(), head.end(), link_re);
            for (auto it = begin; it != std::sregex_iterator(); ++it) {
                std::string tag = it->str();
                std::string rel = attr_value(tag, "rel");
                std::transform(rel.begin(), rel.end(), rel.begin(), ::tolower);
                if (rel == "canonical") { canonical = attr_value(tag, "href"); break; }
            }
        }

        // --- first <h1> ---
        std::string h1 = collapse_ws(strip_tags(find_tag_content(html, "<h1", "</h1>")));

        // --- readable text (scripts/styles removed) ---
        // strip_scripts/strip_tags are plain string ops — safe at any size.
        std::string text = (max_text > 0)
            ? trunc_utf8(collapse_ws(strip_tags(strip_scripts(html))), (size_t)max_text)
            : collapse_ws(strip_tags(strip_scripts(html)));

        // --- outbound links (scan limited portion, no regex on full HTML) ---
        json links = json::array();
        std::set<std::string> seen_links;
        if (max_links > 0) {
            std::string link_scope = html.substr(0, std::min(html.size(), (size_t)300000));
            std::regex a_re(R"xx(<a\b[^>]*href\s*=\s*["']([^"']+)["'])xx", std::regex::icase);
            auto begin = std::sregex_iterator(link_scope.begin(), link_scope.end(), a_re);
            for (auto it = begin; it != std::sregex_iterator()
                                  && (int)links.size() < max_links; ++it) {
                std::string href = resolve_url(target_url, (*it)[1].str());
                if (href.compare(0, 4, "http") != 0) continue;
                if (seen_links.insert(href).second) links.push_back(href);
            }
        }

        out["error"]       = nullptr;
        out["final_domain"] = extract_domain(target_url);
        out["title"]       = title;
        out["h1"]          = h1;
        out["description"] = description;
        out["keywords"]    = keywords;
        out["canonical"]   = canonical;
        out["og"]          = og;
        out["text"]        = text;
        out["links"]       = links;
        out["link_count"]  = (long)links.size();
        return out;
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
                const std::string& region, bool with_oa = false) {
        using clock = std::chrono::steady_clock;

        auto t0 = clock::now();
        std::vector<SearchResult> results;

        if (engine == "google") results = search_google(query, num, region);
        else if (engine == "yandex") results = search_yandex(query, num, region);
        else if (engine == "bing") results = search_bing(query, num, region);
        else if (engine == "openalex") results = search_openalex(query, num);
        else if (engine == "springer") results = search_springer(query, num);
        else if (engine == "core") results = search_core(query, num);
        else if (engine == "semanticscholar" || engine == "s2") results = search_semanticscholar(query, num);
        else if (engine == "doaj") results = search_doaj(query, num);
        else if (engine == "datacite") results = search_datacite(query, num);
        else if (engine == "openaire") results = search_openaire(query, num);
        else if (engine == "base") results = search_base(query, num);
        else if (engine == "cyberleninka") results = search_cyberleninka(query, num);
        else if (engine == "all") results = search_all(query, num, region);
        else results = search_duckduckgo(query, num, region);

        // Optional Unpaywall enrichment: resolve OA full-text links for DOI-bearing results.
        long oa_ms = 0;
        if (with_oa && !results.empty()) {
            auto ta = clock::now();
            enrich_unpaywall(results);
            oa_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - ta).count();
        }

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
        if (with_oa) response["oa_ms"] = oa_ms;

        return response;
    }
};
