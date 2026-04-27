#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct BmpDumpDiag {
    bool use_hw_capture = false;
    bool force_software_active = false;
    int top_black_strip_streak = 0;
    int dxgi_instability_score = 0;
    bool dxgi_disabled_for_session = false;
};

class BmpDumpWriter {
public:
    void configure_from_config();
    void reset_session();
    void dump_capture_if_needed(const std::vector<uint8_t>& frame, int width, int height, const BmpDumpDiag& diag);
    void dump_encode_if_needed(const std::vector<uint8_t>& frame, int width, int height, const BmpDumpDiag& diag);

private:
    static bool dump_rgb24_as_topdown_bmp(const std::string& file_path, const uint8_t* rgb, int width, int height);
    void ensure_output_dir();
    void dump_if_needed(const std::vector<uint8_t>& frame, int width, int height,
                        const BmpDumpDiag& diag, bool capture_stage);

    bool m_enabled = false;
    bool m_dump_capture = false;
    bool m_dump_encode = false;
    uint32_t m_every_n = 1;
    bool m_log = false;
    uint32_t m_log_every_n = 1;
    uint64_t m_max_frames = 0;

    uint64_t m_seq_capture = 0;
    uint64_t m_seq_encode = 0;
    bool m_dir_ready = false;
    std::string m_output_dir;
};
