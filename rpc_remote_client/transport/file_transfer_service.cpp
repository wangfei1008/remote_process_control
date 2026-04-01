#include "transport/file_transfer_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace {

static std::string b64_encode(const std::vector<std::uint8_t>& bytes) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
        i += 3;
    }
    if (i < bytes.size()) {
        const std::uint32_t a = static_cast<std::uint32_t>(bytes[i]);
        const std::uint32_t b = (i + 1 < bytes.size()) ? static_cast<std::uint32_t>(bytes[i + 1]) : 0u;
        const std::uint32_t n = (a << 16) | (b << 8);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        if (i + 1 < bytes.size()) {
            out.push_back(kTable[(n >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

static int b64_lookup(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool b64_decode(const std::string& input, std::vector<std::uint8_t>* out) {
    if (!out) return false;
    out->clear();
    if (input.empty()) return true;
    if ((input.size() % 4) != 0) return false;

    out->reserve((input.size() / 4) * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const char c0 = input[i];
        const char c1 = input[i + 1];
        const char c2 = input[i + 2];
        const char c3 = input[i + 3];

        const int v0 = b64_lookup(static_cast<unsigned char>(c0));
        const int v1 = b64_lookup(static_cast<unsigned char>(c1));
        if (v0 < 0 || v1 < 0) return false;

        const bool pad2 = (c2 == '=');
        const bool pad3 = (c3 == '=');
        const int v2 = pad2 ? 0 : b64_lookup(static_cast<unsigned char>(c2));
        const int v3 = pad3 ? 0 : b64_lookup(static_cast<unsigned char>(c3));
        if ((!pad2 && v2 < 0) || (!pad3 && v3 < 0)) return false;
        if (pad2 && !pad3) return false;

        const std::uint32_t n = (static_cast<std::uint32_t>(v0) << 18) |
                                (static_cast<std::uint32_t>(v1) << 12) |
                                (static_cast<std::uint32_t>(v2) << 6) |
                                static_cast<std::uint32_t>(v3);
        out->push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
        if (!pad2) out->push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        if (!pad3) out->push_back(static_cast<std::uint8_t>(n & 0xFF));
    }
    return true;
}

static bool is_text_preview_ext(const std::string& ext_raw) {
    std::string ext = ext_raw;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".txt" || ext == ".log" || ext == ".ini" || ext == ".json" ||
           ext == ".xml" || ext == ".csv" || ext == ".md" || ext == ".cpp" ||
           ext == ".h" || ext == ".hpp" || ext == ".js" || ext == ".ts" ||
           ext == ".html" || ext == ".css" || ext == ".bat" || ext == ".ps1";
}

static std::filesystem::path path_from_utf8(const std::string& value) {
#if defined(_WIN32)
    return std::filesystem::u8path(value);
#else
    return std::filesystem::path(value);
#endif
}

static std::string utf8_from_path(const std::filesystem::path& p) {
#if defined(_WIN32)
    const auto u8 = p.u8string();
#if defined(__cpp_char8_t)
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
#else
    return u8;
#endif
#else
    return p.string();
#endif
}

} // namespace

FileTransferService::FileTransferService(Options options)
    : m_options(std::move(options)) {
    if (m_options.data_root.empty()) {
        m_options.data_root = std::filesystem::path("D:\\rpc_data");
    }
    std::error_code ec;
    std::filesystem::create_directories(m_options.data_root, ec);
}

bool FileTransferService::can_handle_type(const std::string& type) const {
    return type == "fileList" ||
           type == "filePreview" ||
           type == "fileDownloadInit" ||
           type == "fileDownloadChunk" ||
           type == "fileUploadInit" ||
           type == "fileUploadChunk" ||
           type == "fileUploadCommit";
}

void FileTransferService::handle_message(const std::string& client_id,
                                         const nlohmann::json& request,
                                         const std::shared_ptr<rtc::DataChannel>& channel) {
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

std::string FileTransferService::scoped_transfer_id(const std::string& client_id, const std::string& transfer_id) const {
    return client_id + ":" + transfer_id;
}

std::string FileTransferService::new_transfer_id(const std::string& client_id, const char* prefix) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::scoped_lock lk(m_mutex);
    m_transfer_seq += 1;
    std::ostringstream oss;
    oss << prefix << "_" << client_id << "_" << now_ms << "_" << m_transfer_seq;
    return oss.str();
}

std::filesystem::path FileTransferService::resolve_relative_path(const std::string& relative_path, bool allow_empty) const {
    std::filesystem::path rel = path_from_utf8(relative_path);
    if (relative_path.empty()) {
        if (allow_empty) return m_options.data_root;
        throw std::runtime_error("Path is required");
    }
    if (rel.is_absolute()) {
        throw std::runtime_error("Absolute path is not allowed");
    }
    std::filesystem::path full = (m_options.data_root / rel).lexically_normal();
    if (!is_path_under_root(full)) {
        throw std::runtime_error("Path escapes data root");
    }
    return full;
}

std::filesystem::path FileTransferService::resolve_target_file_path(const std::string& relative_dir, const std::string& file_name) const {
    if (file_name.empty()) {
        throw std::runtime_error("fileName is required");
    }
    std::filesystem::path base = resolve_relative_path(relative_dir, true);
    std::filesystem::path name = path_from_utf8(file_name);
    if (name.has_parent_path() || name.is_absolute()) {
        throw std::runtime_error("Invalid fileName");
    }
    auto full = (base / name).lexically_normal();
    if (!is_path_under_root(full)) {
        throw std::runtime_error("Target path is invalid");
    }
    return full;
}

bool FileTransferService::is_path_under_root(const std::filesystem::path& p) const {
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(m_options.data_root, ec);
    if (ec) return false;
    const auto full = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;
    const auto rel = std::filesystem::relative(full, root, ec);
    if (ec) return false;
    if (rel.empty()) return true;
    const auto rel_str = rel.generic_string();
    if (rel_str == "..") return false;
    if (rel_str.rfind("../", 0) == 0) return false;
    return true;
}

std::uint64_t FileTransferService::to_epoch_ms(std::filesystem::file_time_type ft) const {
    const auto now_file = std::filesystem::file_time_type::clock::now();
    const auto now_sys = std::chrono::system_clock::now();
    const auto delta = ft - now_file;
    const auto sys_tp = now_sys + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        sys_tp.time_since_epoch()).count());
}

std::size_t FileTransferService::clamp_chunk_size(std::size_t value) const {
    constexpr std::size_t kMinChunk = 4 * 1024;
    constexpr std::size_t kMaxChunk = 64 * 1024;
    if (value == 0) value = m_options.default_chunk_size;
    value = std::max(kMinChunk, value);
    value = std::min(kMaxChunk, value);
    return value;
}

void FileTransferService::send_json(const std::shared_ptr<rtc::DataChannel>& channel, const nlohmann::json& payload) const {
    if (!channel) return;
    try {
        channel->send(payload.dump());
    } catch (...) {
    }
}

void FileTransferService::send_error(const std::shared_ptr<rtc::DataChannel>& channel,
                                     const char* op,
                                     const std::string& message,
                                     const std::string& transfer_id) const {
    nlohmann::json err = {
        {"type", "fileError"},
        {"op", op ? op : ""},
        {"message", message}
    };
    if (!transfer_id.empty()) err["transferId"] = transfer_id;
    send_json(channel, err);
}

void FileTransferService::handle_list(const std::string&,
                                      const nlohmann::json& request,
                                      const std::shared_ptr<rtc::DataChannel>& channel) {
    try {
        const std::string rel = request.value("path", "");
        const auto dir = resolve_relative_path(rel, true);
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            send_error(channel, "fileList", "Directory not found");
            return;
        }

        nlohmann::json entries = nlohmann::json::array();
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            const bool is_dir = e.is_directory();
            const auto rel_path = utf8_from_path(std::filesystem::relative(e.path(), m_options.data_root));
            nlohmann::json item = {
                {"name", utf8_from_path(e.path().filename())},
                {"path", rel_path},
                {"isDir", is_dir},
                {"size", is_dir ? 0 : static_cast<std::uint64_t>(e.file_size())},
                {"mtime", to_epoch_ms(e.last_write_time())}
            };
            entries.push_back(std::move(item));
        }

        send_json(channel, {
            {"type", "fileListResult"},
            {"path", rel},
            {"root", utf8_from_path(m_options.data_root)},
            {"entries", entries}
        });
    } catch (const std::exception& e) {
        send_error(channel, "fileList", e.what());
    }
}

void FileTransferService::handle_preview(const std::string&,
                                         const nlohmann::json& request,
                                         const std::shared_ptr<rtc::DataChannel>& channel) {
    try {
        const auto file_path = resolve_relative_path(request.value("path", ""), false);
        if (!std::filesystem::exists(file_path) || std::filesystem::is_directory(file_path)) {
            send_error(channel, "filePreview", "File not found");
            return;
        }
        const std::size_t max_bytes = std::min<std::size_t>(
            static_cast<std::size_t>(request.value("maxBytes", 64 * 1024)),
            256 * 1024);
        const bool text_like = is_text_preview_ext(file_path.extension().string());
        if (!text_like) {
            send_json(channel, {
                {"type", "filePreviewResult"},
                {"path", request.value("path", "")},
                {"previewable", false},
                {"reason", "Unsupported preview type"}
            });
            return;
        }

        std::ifstream in(file_path, std::ios::binary);
        if (!in.is_open()) {
            send_error(channel, "filePreview", "Cannot open file");
            return;
        }
        std::string text;
        text.resize(max_bytes);
        in.read(text.data(), static_cast<std::streamsize>(max_bytes));
        text.resize(static_cast<std::size_t>(in.gcount()));
        send_json(channel, {
            {"type", "filePreviewResult"},
            {"path", request.value("path", "")},
            {"previewable", true},
            {"content", text}
        });
    } catch (const std::exception& e) {
        send_error(channel, "filePreview", e.what());
    }
}

void FileTransferService::handle_download_init(const std::string& client_id,
                                               const nlohmann::json& request,
                                               const std::shared_ptr<rtc::DataChannel>& channel) {
    try {
        const std::string rel_path = request.value("path", "");
        const auto file_path = resolve_relative_path(rel_path, false);
        if (!std::filesystem::exists(file_path) || std::filesystem::is_directory(file_path)) {
            send_error(channel, "fileDownloadInit", "File not found");
            return;
        }

        DownloadTask task;
        task.id = new_transfer_id(client_id, "dl");
        task.file_path = file_path;
        task.file_size = static_cast<std::uint64_t>(std::filesystem::file_size(file_path));
        task.mtime_epoch_ms = to_epoch_ms(std::filesystem::last_write_time(file_path));
        task.chunk_size = clamp_chunk_size(static_cast<std::size_t>(request.value("chunkSize", 0)));

        {
            std::scoped_lock lk(m_mutex);
            m_download_tasks[scoped_transfer_id(client_id, task.id)] = task;
        }

        send_json(channel, {
            {"type", "fileDownloadReady"},
            {"transferId", task.id},
            {"path", rel_path},
            {"fileName", utf8_from_path(file_path.filename())},
            {"fileSize", task.file_size},
            {"mtime", task.mtime_epoch_ms},
            {"chunkSize", task.chunk_size}
        });
    } catch (const std::exception& e) {
        send_error(channel, "fileDownloadInit", e.what());
    }
}

void FileTransferService::handle_download_chunk(const std::string& client_id,
                                                const nlohmann::json& request,
                                                const std::shared_ptr<rtc::DataChannel>& channel) {
    const std::string transfer_id = request.value("transferId", "");
    if (transfer_id.empty()) {
        send_error(channel, "fileDownloadChunk", "transferId is required");
        return;
    }

    DownloadTask task;
    {
        std::scoped_lock lk(m_mutex);
        auto it = m_download_tasks.find(scoped_transfer_id(client_id, transfer_id));
        if (it == m_download_tasks.end()) {
            send_error(channel, "fileDownloadChunk", "Download task not found", transfer_id);
            return;
        }
        task = it->second;
    }

    try {
        std::uint64_t offset = static_cast<std::uint64_t>(request.value("offset", 0LL));
        if (offset >= task.file_size) {
            send_json(channel, {
                {"type", "fileDownloadChunkData"},
                {"transferId", transfer_id},
                {"offset", offset},
                {"size", 0},
                {"isEof", true},
                {"data", ""}
            });
            return;
        }
        const std::size_t req_size = clamp_chunk_size(static_cast<std::size_t>(request.value("size", task.chunk_size)));
        const std::size_t read_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(task.file_size - offset, req_size));

        std::ifstream in(task.file_path, std::ios::binary);
        if (!in.is_open()) {
            send_error(channel, "fileDownloadChunk", "Cannot open source file", transfer_id);
            return;
        }
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        std::vector<std::uint8_t> buf(read_size);
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(read_size));
        const auto got = static_cast<std::size_t>(in.gcount());
        buf.resize(got);

        const std::uint64_t next_offset = offset + static_cast<std::uint64_t>(got);
        const bool eof = (next_offset >= task.file_size);

        send_json(channel, {
            {"type", "fileDownloadChunkData"},
            {"transferId", transfer_id},
            {"offset", offset},
            {"size", got},
            {"nextOffset", next_offset},
            {"isEof", eof},
            {"data", b64_encode(buf)}
        });
    } catch (const std::exception& e) {
        send_error(channel, "fileDownloadChunk", e.what(), transfer_id);
    }
}

void FileTransferService::handle_upload_init(const std::string& client_id,
                                             const nlohmann::json& request,
                                             const std::shared_ptr<rtc::DataChannel>& channel) {
    const std::uint64_t file_size = static_cast<std::uint64_t>(request.value("fileSize", 0LL));
    const std::string relative_dir = request.value("path", "");
    const std::string file_name = request.value("fileName", "");
    const bool overwrite = request.value("overwrite", true);
    const bool resume = request.value("resume", false);
    const std::size_t chunk_size = clamp_chunk_size(static_cast<std::size_t>(request.value("chunkSize", 0)));
    const std::string request_id = request.value("requestId", "");
    std::cout << "[file][upload_init] client=" << client_id
              << " requestId=" << request_id
              << " path=" << relative_dir
              << " fileName=" << file_name
              << " fileSize=" << file_size
              << " chunk=" << chunk_size
              << " resume=" << (resume ? 1 : 0) << std::endl;

    if (file_size == 0) {
        send_error(channel, "fileUploadInit", "fileSize must be greater than 0");
        return;
    }

    try {
        const auto final_path = resolve_target_file_path(relative_dir, file_name);
        std::error_code ec;
        std::filesystem::create_directories(final_path.parent_path(), ec);

        if (!overwrite && std::filesystem::exists(final_path)) {
            send_error(channel, "fileUploadInit", "Target file already exists");
            return;
        }

        const std::string transfer_id = new_transfer_id(client_id, "ul");
        auto temp_path = final_path;
        temp_path += ".rpcpart";

        // 默认按“全新上传”处理，避免历史 .rpcpart 残留导致固定偏移卡住。
        // 仅当前端显式传 resume=true 时才走断点续传。
        std::uint64_t received_bytes = 0;
        if (!resume) {
            if (std::filesystem::exists(temp_path)) {
                std::filesystem::remove(temp_path, ec);
                ec.clear();
            }
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

        UploadTask task;
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

        send_json(channel, {
            {"type", "fileUploadReady"},
            {"requestId", request_id},
            {"transferId", transfer_id},
            {"path", relative_dir},
            {"fileName", file_name},
            {"fileSize", file_size},
            {"chunkSize", chunk_size},
            {"nextOffset", received_bytes}
        });
        std::cout << "[file][upload_ready] client=" << client_id
                  << " transferId=" << transfer_id
                  << " nextOffset=" << received_bytes
                  << std::endl;
    } catch (const std::exception& e) {
        std::cout << "[file][upload_init][error] client=" << client_id
                  << " requestId=" << request_id
                  << " err=" << e.what() << std::endl;
        send_error(channel, "fileUploadInit", e.what());
    }
}

void FileTransferService::handle_upload_chunk(const std::string& client_id,
                                              const nlohmann::json& request,
                                              const std::shared_ptr<rtc::DataChannel>& channel) {
    const std::string transfer_id = request.value("transferId", "");
    if (transfer_id.empty()) {
        send_error(channel, "fileUploadChunk", "transferId is required");
        return;
    }
    const auto key = scoped_transfer_id(client_id, transfer_id);

    UploadTask task;
    {
        std::scoped_lock lk(m_mutex);
        auto it = m_upload_tasks.find(key);
        if (it == m_upload_tasks.end()) {
            send_error(channel, "fileUploadChunk", "Upload task not found", transfer_id);
            return;
        }
        task = it->second;
    }

    try {
        const std::uint64_t offset = static_cast<std::uint64_t>(request.value("offset", 0LL));
        if (offset != task.received_bytes) {
            std::cout << "[file][upload_chunk][offset_mismatch] transferId=" << transfer_id
                      << " offset=" << offset
                      << " expected=" << task.received_bytes << std::endl;
            send_json(channel, {
                {"type", "fileUploadAck"},
                {"transferId", transfer_id},
                {"ok", false},
                {"expectedOffset", task.received_bytes}
            });
            return;
        }

        const std::string b64 = request.value("data", "");
        std::vector<std::uint8_t> bytes;
        if (!b64_decode(b64, &bytes)) {
            send_error(channel, "fileUploadChunk", "Invalid base64 payload", transfer_id);
            return;
        }
        if (bytes.empty()) {
            send_error(channel, "fileUploadChunk", "Empty chunk payload", transfer_id);
            return;
        }

        std::fstream out(task.temp_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!out.is_open()) {
            send_error(channel, "fileUploadChunk", "Cannot open temp file", transfer_id);
            return;
        }
        out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();

        task.received_bytes += static_cast<std::uint64_t>(bytes.size());
        if (task.received_bytes > task.file_size) {
            std::cout << "[file][upload_chunk][overflow] transferId=" << transfer_id
                      << " received=" << task.received_bytes
                      << " fileSize=" << task.file_size << std::endl;
            send_error(channel, "fileUploadChunk", "Uploaded bytes exceed file size", transfer_id);
            return;
        }

        {
            std::scoped_lock lk(m_mutex);
            m_upload_tasks[key] = task;
        }

        if ((task.received_bytes % (128 * 1024)) == 0 || task.received_bytes == task.file_size) {
            std::cout << "[file][upload_progress] transferId=" << transfer_id
                      << " uploaded=" << task.received_bytes
                      << "/" << task.file_size << std::endl;
        }

        send_json(channel, {
            {"type", "fileUploadAck"},
            {"transferId", transfer_id},
            {"ok", true},
            {"uploadedBytes", task.received_bytes},
            {"isComplete", task.received_bytes == task.file_size}
        });
    } catch (const std::exception& e) {
        std::cout << "[file][upload_chunk][error] transferId=" << transfer_id
                  << " err=" << e.what() << std::endl;
        send_error(channel, "fileUploadChunk", e.what(), transfer_id);
    }
}

void FileTransferService::handle_upload_commit(const std::string& client_id,
                                               const nlohmann::json& request,
                                               const std::shared_ptr<rtc::DataChannel>& channel) {
    const std::string transfer_id = request.value("transferId", "");
    if (transfer_id.empty()) {
        send_error(channel, "fileUploadCommit", "transferId is required");
        return;
    }
    const auto key = scoped_transfer_id(client_id, transfer_id);

    UploadTask task;
    {
        std::scoped_lock lk(m_mutex);
        auto it = m_upload_tasks.find(key);
        if (it == m_upload_tasks.end()) {
            send_error(channel, "fileUploadCommit", "Upload task not found", transfer_id);
            return;
        }
        task = it->second;
    }

    if (task.received_bytes != task.file_size) {
        std::cout << "[file][upload_commit][incomplete] transferId=" << transfer_id
                  << " uploaded=" << task.received_bytes
                  << " fileSize=" << task.file_size << std::endl;
        send_error(channel, "fileUploadCommit", "Upload not complete", transfer_id);
        return;
    }

    try {
        std::error_code ec;
        std::filesystem::remove(task.final_path, ec);
        std::filesystem::rename(task.temp_path, task.final_path, ec);
        if (ec) {
            send_error(channel, "fileUploadCommit", ec.message(), transfer_id);
            return;
        }

        {
            std::scoped_lock lk(m_mutex);
            m_upload_tasks.erase(key);
        }
        send_json(channel, {
            {"type", "fileUploadCommitted"},
            {"transferId", transfer_id},
            {"path", utf8_from_path(std::filesystem::relative(task.final_path, m_options.data_root))}
        });
        std::cout << "[file][upload_commit][ok] transferId=" << transfer_id
                  << " finalPath=" << utf8_from_path(task.final_path) << std::endl;
    } catch (const std::exception& e) {
        std::cout << "[file][upload_commit][error] transferId=" << transfer_id
                  << " err=" << e.what() << std::endl;
        send_error(channel, "fileUploadCommit", e.what(), transfer_id);
    }
}
