#include "capture/window_visibility_diagnostics.h"

#include "common/window_ops.h"

#include <sstream>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

void append_reason(std::ostringstream& oss, bool& first, const char* text)
{
    if (!first) oss << "; ";
    first = false;
    oss << text;
}

} // namespace

WindowVisibilityDiagnosis diagnose_window_visibility(HWND hwnd)
{
    WindowVisibilityDiagnosis d;
    d.hwnd = hwnd;
    window_ops wops;
    if (!wops.is_valid(hwnd)) {
        d.reason_summary = "invalid HWND";
        return d;
    }

    d.style = wops.get_style(hwnd);
    d.ex_style = wops.get_ex_style(hwnd);
    d.is_visible_api = wops.is_visible(hwnd);
    d.minimized = IsIconic(hwnd) == TRUE;
    d.layered = (d.ex_style & WS_EX_LAYERED) != 0;
    d.ex_transparent = (d.ex_style & WS_EX_TRANSPARENT) != 0;
    d.ex_tool_window = (d.ex_style & WS_EX_TOOLWINDOW) != 0;
    d.ex_no_activate = (d.ex_style & WS_EX_NOACTIVATE) != 0;
    d.has_owner = !wops.is_ownerless_top_level(hwnd);

    COLORREF ck = 0;
    BYTE alpha = 0;
    DWORD lwa_flags = 0;
    if (d.layered && GetLayeredWindowAttributes(hwnd, &ck, &alpha, &lwa_flags)) {
        d.layered_attrs_valid = true;
        d.layered_alpha = alpha;
        d.layered_flags = lwa_flags;
        d.color_key = ck;
    }

    {
        DWORD cloaked = 0;
        window_ops wops;
        if (wops.query_dwm_attribute_dword(hwnd, DWMWA_CLOAKED, cloaked)) {
            d.cloaked = cloaked;
        }
    }

    DWORD affinity = 0;
    if (GetWindowDisplayAffinity(hwnd, &affinity)) {
        d.display_affinity = affinity;
    }

    RECT cr{};
    if (GetClientRect(hwnd, &cr)) {
        d.client_w = cr.right - cr.left;
        d.client_h = cr.bottom - cr.top;
    }

    std::ostringstream reasons;
    bool first = true;

    if (d.minimized) append_reason(reasons, first, "minimized (IsIconic)");
    if (d.cloaked != 0) {
        if (!first) reasons << "; ";
        first = false;
        reasons << "DWM cloaked (0x" << std::hex << d.cloaked << std::dec << ")";
    }

    if (d.display_affinity == WDA_EXCLUDEFROMCAPTURE) {
        append_reason(reasons, first, "WDA_EXCLUDEFROMCAPTURE (some capture APIs may skip it)");
    } else if (d.display_affinity != 0) {
        if (!first) reasons << "; ";
        first = false;
        reasons << "GetWindowDisplayAffinity=0x" << std::hex << d.display_affinity << std::dec;
    }

    if (d.ex_transparent)
        append_reason(reasons, first, "WS_EX_TRANSPARENT (click-through; often no painted pixels)");
    if (d.layered) {
        if (d.layered_attrs_valid) {
            const bool use_alpha = (d.layered_flags & LWA_ALPHA) != 0;
            const bool use_ck = (d.layered_flags & LWA_COLORKEY) != 0;
            if (use_alpha && d.layered_alpha == 0) {
                append_reason(reasons, first, "WS_EX_LAYERED + LWA_ALPHA with alpha=0 (fully transparent)");
            } else if (use_alpha && d.layered_alpha < 32) {
                if (!first) reasons << "; ";
                first = false;
                reasons << "WS_EX_LAYERED with very low alpha (" << static_cast<int>(d.layered_alpha) << ")";
            } else if (use_ck) {
                append_reason(reasons, first, "WS_EX_LAYERED + LWA_COLORKEY (color-key transparency)");
            } else if (!use_alpha && !use_ck) {
                append_reason(reasons, first,
                              "WS_EX_LAYERED but no LWA flags (likely UpdateLayeredWindow per-pixel alpha)");
            }
        } else {
            append_reason(reasons, first,
                          "WS_EX_LAYERED and GetLayeredWindowAttributes failed (likely UpdateLayeredWindow)");
        }
    }

    if (d.ex_tool_window)
        append_reason(reasons, first, "WS_EX_TOOLWINDOW (often absent from taskbar; easy to miss)");
    if (d.ex_no_activate)
        append_reason(reasons, first, "WS_EX_NOACTIVATE (hard to focus; easy to miss)");
    if (d.has_owner) append_reason(reasons, first, "has GW_OWNER (owned popup; may be inconspicuous)");

    if (d.client_w > 0 && d.client_h > 0 && (d.client_w <= 2 || d.client_h <= 2)) {
        append_reason(reasons, first, "tiny client area; may be nearly invisible");
    }

    if (first) {
        append_reason(
            reasons,
            first,
            "no typical hide-style Win32 flags; may still be empty client, same color as desktop, "
            "or UpdateLayeredWindow not reflected in LWA");
    }

    d.reason_summary = reasons.str();
    return d;
}
