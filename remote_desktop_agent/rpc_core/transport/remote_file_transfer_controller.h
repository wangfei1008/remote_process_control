#pragma once

#include <memory>
#include <string>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "nlohmann/json.hpp"
#include "rtc/rtc.hpp"

#include "transport/remote_file_store_repository.h"

// 阶段性适配器：把旧 FileTransferService 收敛成更窄的控制器接口，
// 以便后续彻底删除旧实现、替换为新的文件传输控制器与仓库分离模块。
class remote_file_transfer_controller {
public:
    struct options {
        std::filesystem::path data_root;
        std::size_t default_chunk_size = 256 * 1024;
    };

    explicit remote_file_transfer_controller(options opts);

    bool can_handle_type(const std::string& type) const;

    void handle_message(const std::string& client_id,
                         const nlohmann::json& request,
                         const std::shared_ptr<rtc::DataChannel>& channel);

private:
    std::string scoped_transfer_id(const std::string& client_id, const std::string& transfer_id) const;
    std::string new_transfer_id(const std::string& client_id, const char* prefix);

    void send_error(const std::shared_ptr<rtc::DataChannel>& channel,
                    const char* op,
                    const std::string& message,
                    const std::string& transfer_id = "") const;
    void send_json(const std::shared_ptr<rtc::DataChannel>& channel, const nlohmann::json& payload) const;

    void handle_list(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_preview(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_download_init(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_download_chunk(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_upload_init(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_upload_chunk(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);
    void handle_upload_commit(const std::string& client_id, const nlohmann::json& request, const std::shared_ptr<rtc::DataChannel>& channel);

private:
    struct download_task {
        std::string id;
        std::filesystem::path file_path;
        std::uint64_t file_size = 0;
        std::uint64_t mtime_epoch_ms = 0;
        std::size_t chunk_size = 0;
    };

    struct upload_task {
        std::string id;
        std::filesystem::path temp_path;
        std::filesystem::path final_path;
        std::uint64_t file_size = 0;
        std::size_t chunk_size = 0;
        std::uint64_t received_bytes = 0;
    };

    std::unique_ptr<remote_file_store_repository> m_repo;
    mutable std::mutex m_mutex;
    std::uint64_t m_transfer_seq = 0;

    std::unordered_map<std::string, download_task> m_download_tasks;
    std::unordered_map<std::string, upload_task> m_upload_tasks;
};

