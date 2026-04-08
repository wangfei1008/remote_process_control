#include "transport/remote_file_transfer_controller.h"

#include "transport/remote_file_store_repository.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

remote_file_transfer_controller::remote_file_transfer_controller(options opts)
{
    remote_file_store_repository::options repo_opts;
    repo_opts.data_root = std::move(opts.data_root);
    repo_opts.default_chunk_size = opts.default_chunk_size;
    m_repo = std::make_unique<remote_file_store_repository>(std::move(repo_opts));
}

bool remote_file_transfer_controller::can_handle_type(const std::string& type) const
{
    return type == "fileList" ||
           type == "filePreview" ||
           type == "fileDownloadInit" ||
           type == "fileDownloadChunk" ||
           type == "fileUploadInit" ||
           type == "fileUploadChunk" ||
           type == "fileUploadCommit";
}

std::string remote_file_transfer_controller::scoped_transfer_id(const std::string& client_id, const std::string& transfer_id) const
{
    return client_id + ":" + transfer_id;
}

std::string remote_file_transfer_controller::new_transfer_id(const std::string& client_id, const char* prefix)
{
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::scoped_lock lk(m_mutex);
    m_transfer_seq += 1;
    std::ostringstream oss;
    oss << prefix << "_" << client_id << "_" << now_ms << "_" << m_transfer_seq;
    return oss.str();
}

void remote_file_transfer_controller::send_json(const std::shared_ptr<rtc::DataChannel>& channel, const nlohmann::json& payload) const
{
    if (!channel) return;
    try {
        channel->send(payload.dump());
    } catch (...) {
    }
}

void remote_file_transfer_controller::send_error(const std::shared_ptr<rtc::DataChannel>& channel,
                                               const char* op,
                                               const std::string& message,
                                               const std::string& transfer_id) const
{
    nlohmann::json err = {
        {"type", "fileError"},
        {"op", op ? op : ""},
        {"message", message}
    };
    if (!transfer_id.empty()) err["transferId"] = transfer_id;
    send_json(channel, err);
}

void remote_file_transfer_controller::handle_message(const std::string& client_id,
                                                     const nlohmann::json& request,
                                                     const std::shared_ptr<rtc::DataChannel>& channel)
{
    const std::string type = request.value("type", "");
    if (type == "fileList") {
        handle_list(client_id, request, channel);
        return;
    }
    if (type == "filePreview") {
        handle_preview(client_id, request, channel);
        return;
    }
    if (type == "fileDownloadInit") {
        handle_download_init(client_id, request, channel);
        return;
    }
    if (type == "fileDownloadChunk") {
        handle_download_chunk(client_id, request, channel);
        return;
    }
    if (type == "fileUploadInit") {
        handle_upload_init(client_id, request, channel);
        return;
    }
    if (type == "fileUploadChunk") {
        handle_upload_chunk(client_id, request, channel);
        return;
    }
    if (type == "fileUploadCommit") {
        handle_upload_commit(client_id, request, channel);
        return;
    }
    send_error(channel, "unknown", "Unsupported file transfer message type");
}

void remote_file_transfer_controller::handle_list(const std::string&,
                                                   const nlohmann::json& request,
                                                   const std::shared_ptr<rtc::DataChannel>& channel)
{
    try {
        const std::string rel = request.value("path", "");
        send_json(channel, m_repo->list_dir(rel));
    } catch (const std::exception& e) {
        send_error(channel, "fileList", e.what());
    }
}

void remote_file_transfer_controller::handle_preview(const std::string&,
                                                      const nlohmann::json& request,
                                                      const std::shared_ptr<rtc::DataChannel>& channel)
{
    try {
        const std::string rel = request.value("path", "");
        const std::size_t max_bytes = static_cast<std::size_t>(request.value("maxBytes", 64 * 1024));
        send_json(channel, m_repo->preview_text_file(rel, max_bytes));
    } catch (const std::exception& e) {
        send_error(channel, "filePreview", e.what());
    }
}

void remote_file_transfer_controller::handle_download_init(const std::string& client_id,
                                                            const nlohmann::json& request,
                                                            const std::shared_ptr<rtc::DataChannel>& channel)
{
    try {
        const std::string rel_path = request.value("path", "");
        auto file_path = m_repo->resolve_relative_path(rel_path, false);
        if (!std::filesystem::exists(file_path) || std::filesystem::is_directory(file_path)) {
            send_error(channel, "fileDownloadInit", "File not found");
            return;
        }

        download_task task;
        task.id = new_transfer_id(client_id, "dl");
        task.file_path = std::move(file_path);
        task.file_size = m_repo->file_size(task.file_path);
        task.mtime_epoch_ms = m_repo->file_mtime_epoch_ms(task.file_path);
        task.chunk_size = m_repo->clamp_chunk_size(static_cast<std::size_t>(request.value("chunkSize", 0)));

        {
            std::scoped_lock lk(m_mutex);
            m_download_tasks[scoped_transfer_id(client_id, task.id)] = task;
        }

        send_json(channel, nlohmann::json{
            {"type", "fileDownloadReady"},
            {"transferId", task.id},
            {"path", rel_path},
            {"fileName", remote_file_store_repository::utf8_from_path(task.file_path.filename())},
            {"fileSize", task.file_size},
            {"mtime", task.mtime_epoch_ms},
            {"chunkSize", task.chunk_size}
        });
    } catch (const std::exception& e) {
        send_error(channel, "fileDownloadInit", e.what());
    }
}

void remote_file_transfer_controller::handle_download_chunk(const std::string& client_id,
                                                             const nlohmann::json& request,
                                                             const std::shared_ptr<rtc::DataChannel>& channel)
{
    const std::string transfer_id = request.value("transferId", "");
    if (transfer_id.empty()) {
        send_error(channel, "fileDownloadChunk", "transferId is required");
        return;
    }

    download_task task;
    {
        std::scoped_lock lk(m_mutex);
        const auto key = scoped_transfer_id(client_id, transfer_id);
        const auto it = m_download_tasks.find(key);
        if (it == m_download_tasks.end()) {
            send_error(channel, "fileDownloadChunk", "Download task not found", transfer_id);
            return;
        }
        task = it->second;
    }

    try {
        const std::uint64_t offset = static_cast<std::uint64_t>(request.value("offset", 0LL));
        if (offset >= task.file_size) {
            send_json(channel, nlohmann::json{
                {"type", "fileDownloadChunkData"},
                {"transferId", transfer_id},
                {"offset", offset},
                {"size", 0},
                {"isEof", true},
                {"data", ""}
            });
            return;
        }

        const std::size_t req_size = m_repo->clamp_chunk_size(static_cast<std::size_t>(request.value("size", task.chunk_size)));

        bool eof = false;
        std::uint64_t next_offset = 0;
        const std::vector<std::uint8_t> buf =
            m_repo->read_file_chunk(task.file_path, offset, req_size, eof, next_offset);

        send_json(channel, nlohmann::json{
            {"type", "fileDownloadChunkData"},
            {"transferId", transfer_id},
            {"offset", offset},
            {"size", buf.size()},
            {"nextOffset", next_offset},
            {"isEof", eof},
            {"data", remote_file_store_repository::b64_encode(buf)}
        });
    } catch (const std::exception& e) {
        send_error(channel, "fileDownloadChunk", e.what(), transfer_id);
    }
}

void remote_file_transfer_controller::handle_upload_init(const std::string& client_id,
                                                          const nlohmann::json& request,
                                                          const std::shared_ptr<rtc::DataChannel>& channel)
{
    const std::uint64_t file_size = static_cast<std::uint64_t>(request.value("fileSize", 0LL));
    const std::string relative_dir = request.value("path", "");
    const std::string file_name = request.value("fileName", "");
    const bool overwrite = request.value("overwrite", true);
    const bool resume = request.value("resume", false);
    const std::size_t chunk_size = m_repo->clamp_chunk_size(static_cast<std::size_t>(request.value("chunkSize", 0)));
    const std::string request_id = request.value("requestId", "");

    if (file_size == 0) {
        send_error(channel, "fileUploadInit", "fileSize must be greater than 0");
        return;
    }

    try {
        const auto final_path = m_repo->resolve_target_file_path(relative_dir, file_name);
        std::error_code ec;
        std::filesystem::create_directories(final_path.parent_path(), ec);
        if (!overwrite && std::filesystem::exists(final_path)) {
            send_error(channel, "fileUploadInit", "Target file already exists");
            return;
        }

        const std::string transfer_id = new_transfer_id(client_id, "ul");
        auto temp_path = final_path;
        temp_path += ".rpcpart";

        std::uint64_t received_bytes = 0;
        if (!resume) {
            if (std::filesystem::exists(temp_path)) std::filesystem::remove(temp_path, ec);
            std::ofstream init_file(temp_path, std::ios::binary | std::ios::trunc);
            init_file.close();
        } else if (std::filesystem::exists(temp_path)) {
            received_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(temp_path));
            if (received_bytes > file_size) {
                std::filesystem::remove(temp_path, ec);
                received_bytes = 0;
                std::ofstream init_file(temp_path, std::ios::binary | std::ios::trunc);
                init_file.close();
            }
        } else {
            std::ofstream init_file(temp_path, std::ios::binary | std::ios::trunc);
            init_file.close();
        }

        upload_task task;
        task.id = transfer_id;
        task.temp_path = temp_path;
        task.final_path = final_path;
        task.file_size = file_size;
        task.chunk_size = chunk_size;
        task.received_bytes = received_bytes;

        {
            std::scoped_lock lk(m_mutex);
            m_upload_tasks[scoped_transfer_id(client_id, transfer_id)] = task;
        }

        send_json(channel, nlohmann::json{
            {"type", "fileUploadReady"},
            {"requestId", request_id},
            {"transferId", transfer_id},
            {"path", relative_dir},
            {"fileName", file_name},
            {"fileSize", file_size},
            {"chunkSize", chunk_size},
            {"nextOffset", received_bytes}
        });
    } catch (const std::exception& e) {
        send_error(channel, "fileUploadInit", e.what());
    }
}

void remote_file_transfer_controller::handle_upload_chunk(const std::string& client_id,
                                                            const nlohmann::json& request,
                                                            const std::shared_ptr<rtc::DataChannel>& channel)
{
    const std::string transfer_id = request.value("transferId", "");
    if (transfer_id.empty()) {
        send_error(channel, "fileUploadChunk", "transferId is required");
        return;
    }

    const auto key = scoped_transfer_id(client_id, transfer_id);
    upload_task task;
    {
        std::scoped_lock lk(m_mutex);
        const auto it = m_upload_tasks.find(key);
        if (it == m_upload_tasks.end()) {
            send_error(channel, "fileUploadChunk", "Upload task not found", transfer_id);
            return;
        }
        task = it->second;
    }

    try {
        const std::uint64_t offset = static_cast<std::uint64_t>(request.value("offset", 0LL));
        if (offset != task.received_bytes) {
            send_json(channel, nlohmann::json{
                {"type", "fileUploadAck"},
                {"transferId", transfer_id},
                {"ok", false},
                {"expectedOffset", task.received_bytes}
            });
            return;
        }

        const std::string b64 = request.value("data", "");
        std::vector<std::uint8_t> bytes;
        if (!remote_file_store_repository::b64_decode(b64, &bytes)) {
            send_error(channel, "fileUploadChunk", "Invalid base64 payload", transfer_id);
            return;
        }
        if (bytes.empty()) {
            send_error(channel, "fileUploadChunk", "Empty chunk payload", transfer_id);
            return;
        }

        m_repo->write_upload_chunk(task.temp_path, offset, bytes);
        task.received_bytes += static_cast<std::uint64_t>(bytes.size());
        if (task.received_bytes > task.file_size) {
            send_error(channel, "fileUploadChunk", "Uploaded bytes exceed file size", transfer_id);
            return;
        }

        {
            std::scoped_lock lk(m_mutex);
            m_upload_tasks[key] = task;
        }

        send_json(channel, nlohmann::json{
            {"type", "fileUploadAck"},
            {"transferId", transfer_id},
            {"ok", true},
            {"uploadedBytes", task.received_bytes},
            {"isComplete", task.received_bytes == task.file_size}
        });
    } catch (const std::exception& e) {
        send_error(channel, "fileUploadChunk", e.what(), transfer_id);
    }
}

void remote_file_transfer_controller::handle_upload_commit(const std::string& client_id,
                                                           const nlohmann::json& request,
                                                           const std::shared_ptr<rtc::DataChannel>& channel)
{
    const std::string transfer_id = request.value("transferId", "");
    if (transfer_id.empty()) {
        send_error(channel, "fileUploadCommit", "transferId is required");
        return;
    }

    const auto key = scoped_transfer_id(client_id, transfer_id);
    upload_task task;
    {
        std::scoped_lock lk(m_mutex);
        const auto it = m_upload_tasks.find(key);
        if (it == m_upload_tasks.end()) {
            send_error(channel, "fileUploadCommit", "Upload task not found", transfer_id);
            return;
        }
        task = it->second;
    }

    if (task.received_bytes != task.file_size) {
        send_error(channel, "fileUploadCommit", "Upload not complete", transfer_id);
        return;
    }

    try {
        m_repo->finalize_upload(task.temp_path, task.final_path);
        {
            std::scoped_lock lk(m_mutex);
            m_upload_tasks.erase(key);
        }

        const std::string rel_final = remote_file_store_repository::utf8_from_path(
            std::filesystem::relative(task.final_path, m_repo->data_root()));

        send_json(channel, nlohmann::json{
            {"type", "fileUploadCommitted"},
            {"transferId", transfer_id},
            {"path", rel_final}
        });
    } catch (const std::exception& e) {
        send_error(channel, "fileUploadCommit", e.what(), transfer_id);
    }
}

