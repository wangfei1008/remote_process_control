#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

// 文件系统仓库：只负责安全路径解析 + 文件 IO + 小块读写。
// 控制器负责协议路由/任务状态机，而仓库负责“落盘/读盘/遍历”等底层能力。
class remote_file_store_repository {
public:
    struct options {
        std::filesystem::path data_root;
        std::size_t default_chunk_size = 256 * 1024;
    };

    explicit remote_file_store_repository(options opts);

    const std::filesystem::path& data_root() const { return m_options.data_root; }

    nlohmann::json list_dir(const std::string& rel_path);
    nlohmann::json preview_text_file(const std::string& rel_path, std::size_t max_bytes);

    std::uint64_t file_size(const std::filesystem::path& abs_path) const;
    std::uint64_t file_mtime_epoch_ms(const std::filesystem::path& abs_path) const;

    std::vector<std::uint8_t> read_file_chunk(const std::filesystem::path& abs_path,
                                                std::uint64_t offset,
                                                std::size_t size,
                                                bool& out_eof,
                                                std::uint64_t& out_next_offset) const;

    // 写入上传临时文件：调用方保证 offset 与收到的已接收字节数一致。
    void write_upload_chunk(const std::filesystem::path& temp_path,
                             std::uint64_t offset,
                             const std::vector<std::uint8_t>& bytes);

    void init_upload_temp_file(const std::filesystem::path& temp_path, bool truncate_if_exists);
    void finalize_upload(const std::filesystem::path& temp_path, const std::filesystem::path& final_path);

    std::filesystem::path resolve_relative_path(const std::string& relative_path, bool allow_empty) const;
    std::filesystem::path resolve_target_file_path(const std::string& relative_dir, const std::string& file_name) const;

    std::size_t clamp_chunk_size(std::size_t value) const;

    // 协议打包所需：把文件名/路径转成前端可用的 UTF-8 字符串。
    static std::string utf8_from_path(const std::filesystem::path& p);
    static std::string b64_encode(const std::vector<std::uint8_t>& bytes);
    static bool b64_decode(const std::string& input, std::vector<std::uint8_t>* out);

private:
    static bool is_text_preview_ext(const std::string& ext_raw);
    static std::filesystem::path path_from_utf8(const std::string& value);
    static bool is_path_under_root(const std::filesystem::path& data_root, const std::filesystem::path& p);
    static std::uint64_t to_epoch_ms(std::filesystem::file_time_type ft);

    options m_options;
};

