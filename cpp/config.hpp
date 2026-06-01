#pragma once
#include <string>
#include <fstream>
#include <unordered_map>
#include <cstdlib>

struct Config {
    // Proxy
    std::string mobile_user, mobile_pass, mobile_port;
    std::string region_ru_host, region_kz_host, region_global_host;
    std::string google_user, google_pass, google_host, google_port;

    // API keys
    std::string yandex_api_key, yandex_folder_id;

    void load(const std::string& env_path) {
        std::unordered_map<std::string, std::string> env;
        std::ifstream f(env_path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            env[line.substr(0, eq)] = line.substr(eq + 1);
        }

        auto get = [&](const std::string& key, const std::string& def = "") {
            auto it = env.find(key);
            return it != env.end() ? it->second : def;
        };

        mobile_user = get("DECODO_MOBILE_USER");
        mobile_pass = get("DECODO_MOBILE_PASS");
        mobile_port = get("DECODO_MOBILE_PORT", "40001");

        region_ru_host = get("DECODO_REGION_RU_HOST", "ru.decodo.com");
        region_kz_host = get("DECODO_REGION_KZ_HOST", "kz.decodo.com");
        region_global_host = get("DECODO_REGION_GLOBAL_HOST", "gate.decodo.com");

        google_user = get("DECODO_GOOGLE_USER");
        google_pass = get("DECODO_GOOGLE_PASS");
        google_host = get("DECODO_GOOGLE_HOST", "gate.decodo.com");
        google_port = get("DECODO_GOOGLE_PORT", "10000");

        yandex_api_key = get("YANDEX_API_KEY");
        yandex_folder_id = get("YANDEX_FOLDER_ID");
    }

    std::string proxy_url(const std::string& region) const {
        std::string host = region_kz_host;
        if (region == "ru") host = region_ru_host;
        else if (region == "global") host = region_global_host;
        return "http://" + mobile_user + ":" + mobile_pass + "@" + host + ":" + mobile_port;
    }

    std::string google_proxy_url() const {
        return "http://" + google_user + ":" + google_pass + "@" + google_host + ":" + google_port;
    }

    // Country-targeted rotating residential exit (base gate :7000). Each request gets a
    // fresh in-country IP; combined with browser headers this clears Startpage's anti-bot.
    std::string google_scrape_proxy_url(const std::string& country = "us") const {
        return "http://user-" + google_user + "-country-" + country + ":" + google_pass +
               "@" + region_global_host + ":7000";
    }
};
