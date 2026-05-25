#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <curl/curl.h>

static size_t write_cb(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

struct HttpResponse {
    long status = 0;
    std::string body;
};

// Connection pool per host — reuses TCP+TLS connections
class HttpClient {
    // Pool of CURL handles per host
    struct Pool {
        std::mutex mtx;
        std::vector<CURL*> handles;
    };

    std::mutex pools_mtx;
    std::unordered_map<std::string, Pool> pools;

    static std::string host_key(const std::string& url) {
        auto start = url.find("://");
        if (start == std::string::npos) return url;
        start += 3;
        auto end = url.find('/', start);
        return url.substr(0, end == std::string::npos ? url.size() : end);
    }

    CURL* acquire(const std::string& url) {
        auto key = host_key(url);
        std::lock_guard<std::mutex> g(pools_mtx);
        auto& pool = pools[key];
        std::lock_guard<std::mutex> pg(pool.mtx);
        if (!pool.handles.empty()) {
            CURL* h = pool.handles.back();
            pool.handles.pop_back();
            return h;
        }
        return curl_easy_init();
    }

    void release(const std::string& url, CURL* curl) {
        auto key = host_key(url);
        curl_easy_reset(curl);
        std::lock_guard<std::mutex> g(pools_mtx);
        auto& pool = pools[key];
        std::lock_guard<std::mutex> pg(pool.mtx);
        if (pool.handles.size() < 8) { // max 8 per host
            pool.handles.push_back(curl);
        } else {
            curl_easy_cleanup(curl);
        }
    }

    void setup_common(CURL* curl, const std::string& url, std::string* body, long timeout_ms) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        // Enable TCP keepalive & connection reuse
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
        // DNS cache (per-handle, survives reset with pool)
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 300L);
    }

public:
    HttpClient() { curl_global_init(CURL_GLOBAL_ALL); }

    ~HttpClient() {
        for (auto& [k, pool] : pools) {
            for (auto* h : pool.handles) curl_easy_cleanup(h);
        }
        curl_global_cleanup();
    }

    HttpResponse get(const std::string& url, const std::string& proxy = "",
                     long timeout_ms = 15000) {
        HttpResponse resp;
        CURL* curl = acquire(url);
        if (!curl) return resp;

        setup_common(curl, url, &resp.body, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131.0.0.0 Safari/537.36");
        if (!proxy.empty())
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

        release(url, curl);
        return resp;
    }

    HttpResponse post_json(const std::string& url, const std::string& json_body,
                           const std::string& auth_header = "",
                           long timeout_ms = 15000) {
        HttpResponse resp;
        CURL* curl = acquire(url);
        if (!curl) return resp;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!auth_header.empty())
            headers = curl_slist_append(headers, auth_header.c_str());

        setup_common(curl, url, &resp.body, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

        curl_slist_free_all(headers);
        release(url, curl);
        return resp;
    }

    HttpResponse post_form(const std::string& url, const std::string& form_data,
                           const std::string& proxy = "", long timeout_ms = 20000) {
        HttpResponse resp;
        CURL* curl = acquire(url);
        if (!curl) return resp;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

        setup_common(curl, url, &resp.body, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form_data.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131.0.0.0 Safari/537.36");
        if (!proxy.empty())
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

        curl_slist_free_all(headers);
        release(url, curl);
        return resp;
    }
};
