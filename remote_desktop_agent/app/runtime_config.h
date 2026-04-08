#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>

namespace runtime_config {

inline std::string trim(std::string value)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

inline std::unordered_map<std::string, std::string> load_key_values()
{
    std::unordered_map<std::string, std::string> map;
    std::ifstream input;
    const char* candidates[] = {
        "rpc_config.ini",
        "./rpc_config.ini",
        "../rpc_config.ini",
        "../../rpc_config.ini",
        "rpc_remote_client/rpc_config.ini",
        "../rpc_remote_client/rpc_config.ini",
        "../../rpc_remote_client/rpc_config.ini",
        "remote_process_control/rpc_config.ini",
        "../remote_process_control/rpc_config.ini",
        "../../remote_process_control/rpc_config.ini",
    };
    for (const char* c : candidates) {
        input.open(c);
        if (input.is_open()) break;
        input.clear();
    }
    if (!input.is_open()) return map;

    std::string line;
    while (std::getline(input, line)) {
        const auto comment_pos = line.find_first_of("#;");
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        line = trim(line);
        if (line.empty()) continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (!key.empty()) map[key] = value;
    }
    return map;
}

inline std::string get_string(const char* key, const std::string& default_value = "")
{
    const auto map = load_key_values();
    const auto it = map.find(key);
    if (it != map.end()) {
        return it->second;
    }
    return default_value;
}

inline int get_int(const char* key, int default_value)
{
    const std::string value = get_string(key, "");
    if (value.empty()) return default_value;
    return std::atoi(value.c_str());
}

inline bool get_bool(const char* key, bool default_value)
{
    std::string value = get_string(key, "");
    if (value.empty()) return default_value;

    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
    return default_value;
}

}

