#pragma once

#include "nlohmann/json.hpp"
#include "rtc/rtc.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class FileTransferService {
public:
    struct Options {
        std::filesystem::path data_root;
        std::size_t default_chunk_size = 256 * 1024;
    };

    explicit FileTransferService(Options options);

    bool can_handle_type(const std::string& type) const;
    void handle_message(const std::string& client_id,
                        const nlohmann::json& request,
                        const std::shared_ptr<rtc::DataChannel>& channel);

private:
    struct DownloadTask {
        std::string id;
        std::filesystem::path file_path;
        std::uint64_t file_size = 0;
        std::uint64_t mtime_epoch_ms = 0;
        std::size_t chunk_size = 0;
    };

    struct UploadTask {
        std::string id;
        std::filesystem::path temp_path;
        std::filesystem::path final_path;
        std::uint64_t file_size = 0;
        std::size_t chunk_size = 0;
        std::uint64_t received_bytes = 0;
    };

    std::string scoped_transfer_id(const std::string& client_id, const std::string& transfer_id) const;
    std::string new_transfer_id(const std::string& client_id, const char* prefix);

    std::filesystem::path resolve_relative_path(const std::string& relative_path, bool allow_empty) const;
    std::filesystem::path resolve_target_file_path(const std::string& relative_dir, const std::string& file_name) const;
    bool is_path_under_root(const std::filesystem::path& p) const;
    std::uint64_t to_epoch_ms(std::filesystem::file_time_type ft) const;
    std::size_t clamp_chunk_size(std::size_t value) const;

    void send_json(const std::shared_ptr<rtc::DataChannel>& channel, const nlohmann::json& payload) const;
    void send_error(const std::shared_ptr<rtc::DataChannel>& channel,
                    const char* op,
                    const std::string& message,
                    const std::string& transfer_id = "") const;

    void handle_list(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_preview(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_download_init(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_download_chunk(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_upload_init(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_upload_chunk(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_upload_commit(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);

private:
    Options m_options;
    std::mutex m_mutex;
    std::uint64_t m_transfer_seq = 0;
    std::unordered_map<std::string, DownloadTask> m_download_tasks;
    std::unordered_map<std::string, UploadTask> m_upload_tasks;
};
