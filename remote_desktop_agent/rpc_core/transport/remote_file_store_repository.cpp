#include "transport/remote_file_store_repository.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <sstream>

#include "app/runtime_config.h"

remote_file_store_repository::remote_file_store_repository(options opts)
    : m_options(std::move(opts))
{
    if (m_options.data_root.empty()) {
        m_options.data_root = std::filesystem::path(
            runtime_config::get_string("RPC_DATA_ROOT", "D:\\rpc_data"));
    }
    std::error_code ec;
    std::filesystem::create_directories(m_options.data_root, ec);
}

bool remote_file_store_repository::is_text_preview_ext(const std::string& ext_raw)
{
    std::string ext = ext_raw;
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".txt" || ext == ".log" || ext == ".ini" || ext == ".json" ||
           ext == ".xml" || ext == ".csv" || ext == ".md" || ext == ".cpp" ||
           ext == ".h" || ext == ".hpp" || ext == ".js" || ext == ".ts" ||
           ext == ".html" || ext == ".css" || ext == ".bat" || ext == ".ps1";
}

std::filesystem::path remote_file_store_repository::path_from_utf8(const std::string& value)
{
#if defined(_WIN32)
    // On Windows, this preserves UTF-8 → UTF-16 conversion behavior.
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),
                                               reinterpret_cast<const char8_t*>(value.data() + value.size())));
#else
    return std::filesystem::path(value);
#endif
}

std::string remote_file_store_repository::utf8_from_path(const std::filesystem::path& p)
{
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

bool remote_file_store_repository::is_path_under_root(const std::filesystem::path& data_root,
                                                      const std::filesystem::path& p)
{
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(data_root, ec);
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

std::uint64_t remote_file_store_repository::to_epoch_ms(std::filesystem::file_time_type ft)
{
    const auto now_file = std::filesystem::file_time_type::clock::now();
    const auto now_sys = std::chrono::system_clock::now();
    const auto delta = ft - now_file;
    const auto sys_tp = now_sys + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        sys_tp.time_since_epoch()).count());
}

std::string remote_file_store_repository::b64_encode(const std::vector<std::uint8_t>& bytes)
{
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                 (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                 (static_cast<std::uint32_t>(bytes[i + 2]));
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

static int b64_lookup(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool remote_file_store_repository::b64_decode(const std::string& input, std::vector<std::uint8_t>* out)
{
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

std::filesystem::path remote_file_store_repository::resolve_relative_path(const std::string& relative_path, bool allow_empty) const
{
    std::filesystem::path rel = path_from_utf8(relative_path);
    if (relative_path.empty()) {
        if (allow_empty) return m_options.data_root;
        throw std::runtime_error("Path is required");
    }
    if (rel.is_absolute()) {
        throw std::runtime_error("Absolute path is not allowed");
    }
    std::filesystem::path full = (m_options.data_root / rel).lexically_normal();
    if (!is_path_under_root(m_options.data_root, full)) {
        throw std::runtime_error("Path escapes data root");
    }
    return full;
}

std::filesystem::path remote_file_store_repository::resolve_target_file_path(const std::string& relative_dir, const std::string& file_name) const
{
    if (file_name.empty()) {
        throw std::runtime_error("fileName is required");
    }
    std::filesystem::path base = resolve_relative_path(relative_dir, true);
    std::filesystem::path name = path_from_utf8(file_name);
    if (name.has_parent_path() || name.is_absolute()) {
        throw std::runtime_error("Invalid fileName");
    }
    auto full = (base / name).lexically_normal();
    if (!is_path_under_root(m_options.data_root, full)) {
        throw std::runtime_error("Target path is invalid");
    }
    return full;
}

std::size_t remote_file_store_repository::clamp_chunk_size(std::size_t value) const
{
    constexpr std::size_t kMinChunk = 4 * 1024;
    constexpr std::size_t kMaxChunk = 64 * 1024;
    if (value == 0) value = m_options.default_chunk_size;
    value = std::max(kMinChunk, value);
    value = std::min(kMaxChunk, value);
    return value;
}

std::uint64_t remote_file_store_repository::file_size(const std::filesystem::path& abs_path) const
{
    return static_cast<std::uint64_t>(std::filesystem::file_size(abs_path));
}

std::uint64_t remote_file_store_repository::file_mtime_epoch_ms(const std::filesystem::path& abs_path) const
{
    return to_epoch_ms(std::filesystem::last_write_time(abs_path));
}

nlohmann::json remote_file_store_repository::list_dir(const std::string& rel_path)
{
    const auto dir = resolve_relative_path(rel_path, true);
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        throw std::runtime_error("Directory not found");
    }

    nlohmann::json entries = nlohmann::json::array();
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const bool is_dir = e.is_directory();
        const auto item_rel_path = utf8_from_path(std::filesystem::relative(e.path(), m_options.data_root));
        nlohmann::json item = {
            {"name", utf8_from_path(e.path().filename())},
            {"path", item_rel_path},
            {"isDir", is_dir},
            {"size", is_dir ? 0 : static_cast<std::uint64_t>(e.file_size())},
            {"mtime", to_epoch_ms(e.last_write_time())}
        };
        entries.push_back(std::move(item));
    }

    return nlohmann::json{
        {"type", "fileListResult"},
        {"path", rel_path},
        {"root", utf8_from_path(m_options.data_root)},
        {"entries", entries}
    };
}

nlohmann::json remote_file_store_repository::preview_text_file(const std::string& rel_path, std::size_t max_bytes)
{
    const auto file_path = resolve_relative_path(rel_path, false);
    if (!std::filesystem::exists(file_path) || std::filesystem::is_directory(file_path)) {
        throw std::runtime_error("File not found");
    }

    const std::size_t bounded_max = std::min<std::size_t>(max_bytes, 256 * 1024);
    const bool text_like = is_text_preview_ext(file_path.extension().string());
    if (!text_like) {
        return nlohmann::json{
            {"type", "filePreviewResult"},
            {"path", rel_path},
            {"previewable", false},
            {"reason", "Unsupported preview type"}
        };
    }

    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file");
    }

    std::string text;
    text.resize(bounded_max);
    in.read(text.data(), static_cast<std::streamsize>(bounded_max));
    text.resize(static_cast<std::size_t>(in.gcount()));

    return nlohmann::json{
        {"type", "filePreviewResult"},
        {"path", rel_path},
        {"previewable", true},
        {"content", text}
    };
}

std::vector<std::uint8_t> remote_file_store_repository::read_file_chunk(const std::filesystem::path& abs_path,
                                                                           std::uint64_t offset,
                                                                           std::size_t size,
                                                                           bool& out_eof,
                                                                           std::uint64_t& out_next_offset) const
{
    const auto fsize = static_cast<std::uint64_t>(std::filesystem::file_size(abs_path));
    if (offset >= fsize) {
        out_eof = true;
        out_next_offset = offset;
        return {};
    }

    const std::size_t read_size = static_cast<std::size_t>(std::min<std::uint64_t>(fsize - offset, size));
    std::ifstream in(abs_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open source file");
    }
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    std::vector<std::uint8_t> buf(read_size);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(read_size));
    const std::size_t got = static_cast<std::size_t>(in.gcount());
    buf.resize(got);

    out_next_offset = offset + static_cast<std::uint64_t>(got);
    out_eof = (out_next_offset >= fsize);
    return buf;
}

void remote_file_store_repository::write_upload_chunk(const std::filesystem::path& temp_path,
                                                        std::uint64_t offset,
                                                        const std::vector<std::uint8_t>& bytes)
{
    std::fstream out(temp_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot open temp file");
    }
    out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
}

void remote_file_store_repository::init_upload_temp_file(const std::filesystem::path& temp_path, bool truncate_if_exists)
{
    std::error_code ec;
    if (std::filesystem::exists(temp_path) && truncate_if_exists) {
        std::filesystem::remove(temp_path, ec);
        ec.clear();
    }
    std::ofstream init_file(temp_path, std::ios::binary | std::ios::trunc);
    init_file.close();
}

void remote_file_store_repository::finalize_upload(const std::filesystem::path& temp_path, const std::filesystem::path& final_path)
{
    std::error_code ec;
    std::filesystem::remove(final_path, ec);
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        throw std::runtime_error(ec.message());
    }
}

