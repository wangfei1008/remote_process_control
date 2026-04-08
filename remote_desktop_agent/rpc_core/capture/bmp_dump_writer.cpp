#include "capture/bmp_dump_writer.h"
#include "app/runtime_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>

#include <windows.h>

bool BmpDumpWriter::dump_rgb24_as_topdown_bmp(const std::string& file_path, const uint8_t* rgb, int width, int height)
{
    if (!rgb || width <= 0 || height <= 0) return false;

    const int row_stride = ((width * 3 + 3) & ~3);
    const uint32_t pixel_data_size = static_cast<uint32_t>(row_stride) * static_cast<uint32_t>(height);

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    file_header.bfSize = file_header.bfOffBits + pixel_data_size;

    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = width;
    info_header.biHeight = -height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 24;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = pixel_data_size;

    std::ofstream output_stream(file_path, std::ios::binary);
    if (!output_stream) return false;

    output_stream.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    output_stream.write(reinterpret_cast<const char*>(&info_header), sizeof(info_header));

    std::vector<uint8_t> row(static_cast<size_t>(row_stride), 0);
    for (int y = 0; y < height; ++y) {
        std::fill(row.begin(), row.end(), 0);
        const size_t row_base = static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        for (int x = 0; x < width; ++x) {
            const size_t idx = row_base + static_cast<size_t>(x) * 3u;
            const uint8_t r = rgb[idx + 0];
            const uint8_t g = rgb[idx + 1];
            const uint8_t b = rgb[idx + 2];
            row[static_cast<size_t>(x) * 3u + 0] = b;
            row[static_cast<size_t>(x) * 3u + 1] = g;
            row[static_cast<size_t>(x) * 3u + 2] = r;
        }
        output_stream.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row_stride));
    }
    return output_stream.good();
}

void BmpDumpWriter::configure_from_config()
{
    m_enabled = runtime_config::get_bool("RPC_DUMP_BMP", false);
    if (!m_enabled) return;

    std::string stage = runtime_config::get_string("RPC_DUMP_BMP_STAGE", "capture");
    std::transform(stage.begin(), stage.end(), stage.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (stage == "encode") {
        m_dump_capture = false;
        m_dump_encode = true;
    } else if (stage == "both") {
        m_dump_capture = true;
        m_dump_encode = true;
    } else {
        m_dump_capture = true;
        m_dump_encode = false;
    }

    m_every_n = (std::max)(1u, static_cast<uint32_t>(runtime_config::get_int("RPC_DUMP_BMP_EVERY_N", static_cast<int>(m_every_n))));
    m_log = runtime_config::get_bool("RPC_DUMP_BMP_LOG", m_log);
    m_log_every_n = (std::max)(1u, static_cast<uint32_t>(runtime_config::get_int("RPC_DUMP_BMP_LOG_EVERY_N", static_cast<int>(m_log_every_n))));
    m_max_frames = static_cast<uint64_t>((std::max)(0, runtime_config::get_int("RPC_DUMP_BMP_MAX_FRAMES", static_cast<int>(m_max_frames))));

    std::cout << "[dumpbmp] enabled stage=" << (m_dump_capture ? "capture" : "")
              << (m_dump_capture && m_dump_encode ? "+" : "")
              << (m_dump_encode ? "encode" : "")
              << " every_n=" << m_every_n
              << " log=" << (m_log ? 1 : 0)
              << " log_every_n=" << m_log_every_n
              << " max_frames=" << m_max_frames
              << std::endl;
}

void BmpDumpWriter::reset_session()
{
    m_seq_capture = 0;
    m_seq_encode = 0;
}

void BmpDumpWriter::dump_capture_if_needed(const std::vector<uint8_t>& frame, int width, int height, const BmpDumpDiag& diag)
{
    if (!m_enabled || !m_dump_capture) return;
    dump_if_needed(frame, width, height, diag, true);
}

void BmpDumpWriter::dump_encode_if_needed(const std::vector<uint8_t>& frame, int width, int height, const BmpDumpDiag& diag)
{
    if (!m_enabled || !m_dump_encode) return;
    dump_if_needed(frame, width, height, diag, false);
}

void BmpDumpWriter::ensure_output_dir()
{
    if (m_dir_ready) return;
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    m_output_dir = std::string(cwd) + "\\image";
    CreateDirectoryA(m_output_dir.c_str(), nullptr);
    m_dir_ready = true;
}

void BmpDumpWriter::dump_if_needed(const std::vector<uint8_t>& frame, int width, int height,
                                   const BmpDumpDiag& diag, bool capture_stage)
{
    if (frame.empty() || width <= 0 || height <= 0) return;

    uint64_t& seq = capture_stage ? m_seq_capture : m_seq_encode;
    const uint64_t idx = seq++;
    if (m_max_frames != 0 && idx >= m_max_frames) return;
    if (m_every_n > 0 && (idx % m_every_n) != 0) return;

    ensure_output_dir();
    const std::string backend_tag = diag.use_hw_capture ? "dxgi" : "gdi";
    const std::string stage = capture_stage ? "cap_" : "enc_";
    const std::string file_path = m_output_dir + "\\" + stage + std::to_string(idx) + "_" + backend_tag + ".bmp";
    dump_rgb24_as_topdown_bmp(file_path, frame.data(), width, height);

    if (!m_log || m_log_every_n == 0 || (idx % m_log_every_n) != 0) return;

    std::cout << (capture_stage ? "[dumpbmp][cap] " : "[dumpbmp][enc] ")
              << "idx=" << idx
              << " backend=" << backend_tag
              << " forceSoftwareActive=" << (diag.force_software_active ? 1 : 0)
              << " topBlackStripStreak=" << diag.top_black_strip_streak
              << " dxgiInstabilityScore=" << diag.dxgi_instability_score
              << " dxgiDisabledForSession=" << (diag.dxgi_disabled_for_session ? 1 : 0)
              << std::endl;
}

