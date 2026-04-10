#pragma once

#include <windows.h>

#include <string>

// Heuristic hints for “IsWindowVisible but hard to notice” (not a perfect match for human perception).
struct WindowVisibilityDiagnosis {
    HWND hwnd = nullptr;
    LONG_PTR style = 0;
    LONG_PTR ex_style = 0;
    bool is_visible_api = false;
    bool minimized = false;
    bool layered = false;
    bool layered_attrs_valid = false;
    BYTE layered_alpha = 255;
    DWORD layered_flags = 0;
    COLORREF color_key = 0;
    bool ex_transparent = false;
    bool ex_tool_window = false;
    bool ex_no_activate = false;
    bool has_owner = false;
    DWORD cloaked = 0;
    DWORD display_affinity = 0;
    int client_w = 0;
    int client_h = 0;

    // English phrases, semicolon-separated, for console logs.
    std::string reason_summary;
};

WindowVisibilityDiagnosis diagnose_window_visibility(HWND hwnd);
