// ============================================================
// dxgi_capture_backend.cpp
// DXGI Desktop Duplication 实现
// ============================================================

#include "dxgi_capture_backend.h"
#include "../infra/win32_window.h"

#if defined(_WIN32)
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace capture {

// ============================================================
// Impl
// ============================================================
struct DxgiCaptureBackend::Impl {
    Impl();
    ~Impl();

    bool is_available()  const { return m_available; }
    bool last_timed_out() const { return m_last_timed_out; }

    // 开始采集：选定输出器并 AcquireNextFrame
    bool begin(const std::vector<HWND>& hwnds);

    // 把已 acquire 的帧裁剪到指定窗口，返回 RGB24
    std::vector<uint8_t> copy_window_rgb(HWND hwnd,
                                         int& out_w, int& out_h,
                                         int& out_left, int& out_top);

    // 释放当前帧
    void end();

    // 失败计数（连续失败 N 次后触发 reset）
    void note_failure(bool& out_should_reset);
    void note_success();
    void reset_counters();

    void full_reset();   // 重建 duplication

private:
    bool init();
    bool init_output(UINT index);
    bool ensure_output_for_window(HWND hwnd);
    bool ensure_staging(int w, int h);
    bool acquire_frame();
    void release_duplication();

    static constexpr int kFailResetThreshold = 6;

    bool m_available      = false;
    UINT m_output_index   = 0;
    int  m_output_w       = 0;
    int  m_output_h       = 0;
    RECT m_output_rect{};

    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_last_frame;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_staging;

    bool m_last_timed_out   = false;
    int  m_fail_streak      = 0;
};

// ---- Impl 实现 ----------------------------------------------

DxgiCaptureBackend::Impl::Impl()  { m_available = init(); }
DxgiCaptureBackend::Impl::~Impl() { release_duplication(); }

bool DxgiCaptureBackend::Impl::init() {
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL out_level{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &m_device, &out_level, &m_context);
    if (FAILED(hr) || !m_device || !m_context) return false;
    return init_output(0);
}

bool DxgiCaptureBackend::Impl::init_output(UINT index) {
    if (!m_device) return false;
    release_duplication();

    Microsoft::WRL::ComPtr<IDXGIDevice>  dxgi_dev;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(m_device.As(&dxgi_dev)))          return false;
    if (FAILED(dxgi_dev->GetAdapter(&adapter)))  return false;

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(index, &output)) || !output) return false;

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    m_output_rect  = desc.DesktopCoordinates;
    m_output_w     = m_output_rect.right  - m_output_rect.left;
    m_output_h     = m_output_rect.bottom - m_output_rect.top;
    if (m_output_w <= 0 || m_output_h <= 0) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) return false;
    if (FAILED(output1->DuplicateOutput(m_device.Get(), &m_duplication))) return false;

    m_output_index = index;
    m_staging.Reset();
    return true;
}

void DxgiCaptureBackend::Impl::release_duplication() {
    if (m_duplication) m_duplication->ReleaseFrame();
    m_duplication.Reset();
    m_last_frame.Reset();
}

bool DxgiCaptureBackend::Impl::ensure_output_for_window(HWND hwnd) {
    if (!hwnd) return false;
    win32::Window wops;
    RECT wr{};
    if (!wops.get_effective_rect(hwnd, wr)) return false;

    const LONG cx = (wr.left + wr.right)  / 2;
    const LONG cy = (wr.top  + wr.bottom) / 2;

    // 已在当前输出器范围内
    if (m_duplication
        && cx >= m_output_rect.left && cx < m_output_rect.right
        && cy >= m_output_rect.top  && cy < m_output_rect.bottom) {
        return true;
    }

    // 搜索正确的输出器
    Microsoft::WRL::ComPtr<IDXGIDevice>  dxgi_dev;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(m_device.As(&dxgi_dev)))         return false;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) return false;

    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIOutput> out;
        if (adapter->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) break;
        if (!out) continue;
        DXGI_OUTPUT_DESC d{};
        out->GetDesc(&d);
        if (cx >= d.DesktopCoordinates.left && cx < d.DesktopCoordinates.right
         && cy >= d.DesktopCoordinates.top  && cy < d.DesktopCoordinates.bottom) {
            return init_output(i);
        }
    }
    return init_output(0);
}

bool DxgiCaptureBackend::Impl::ensure_staging(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (m_staging) {
        D3D11_TEXTURE2D_DESC d{};
        m_staging->GetDesc(&d);
        if (static_cast<int>(d.Width) == w && static_cast<int>(d.Height) == h) return true;
        m_staging.Reset();
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width            = static_cast<UINT>(w);
    d.Height           = static_cast<UINT>(h);
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_STAGING;
    d.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(m_device->CreateTexture2D(&d, nullptr, &m_staging)) && m_staging;
}

bool DxgiCaptureBackend::Impl::acquire_frame() {
    if (!m_duplication) return false;
    m_last_timed_out = false;
    m_last_frame.Reset();

    DXGI_OUTDUPL_FRAME_INFO fi{};
    Microsoft::WRL::ComPtr<IDXGIResource> res;

    // 1 ms 超时，非阻塞采集
    constexpr UINT kTimeoutMs = 1;
    HRESULT hr = m_duplication->AcquireNextFrame(kTimeoutMs, &fi, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) { m_last_timed_out = true; return false; }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        if (!init_output(m_output_index)) return false;
        hr = m_duplication->AcquireNextFrame(kTimeoutMs, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { m_last_timed_out = true; return false; }
    }
    if (FAILED(hr) || !res) return false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) { m_duplication->ReleaseFrame(); return false; }
    m_last_frame = tex;
    return true;
}

std::vector<uint8_t> DxgiCaptureBackend::Impl::copy_window_rgb(
    HWND hwnd, int& out_w, int& out_h, int& out_left, int& out_top)
{
    out_w = out_h = out_left = out_top = 0;
    win32::Window wops;
    if (!m_available || !wops.is_valid(hwnd) || !m_last_frame) return {};

    RECT wr{};
    if (!wops.get_effective_rect(hwnd, wr)) return {};

    // 裁剪到当前输出器范围
    RECT clipped{
        max(wr.left,   m_output_rect.left),
        max(wr.top,    m_output_rect.top),
        min(wr.right,  m_output_rect.right),
        min(wr.bottom, m_output_rect.bottom)
    };
    int w = (clipped.right  - clipped.left) & ~1;
    int h = (clipped.bottom - clipped.top)  & ~1;
    if (w <= 0 || h <= 0) return {};
    if (!ensure_staging(w, h)) return {};

    D3D11_BOX box{};
    box.left   = static_cast<UINT>(clipped.left - m_output_rect.left);
    box.top    = static_cast<UINT>(clipped.top  - m_output_rect.top);
    box.right  = box.left + static_cast<UINT>(w);
    box.bottom = box.top  + static_cast<UINT>(h);
    box.front  = 0; box.back = 1;

    m_context->CopySubresourceRegion(
        m_staging.Get(), 0, 0, 0, 0, m_last_frame.Get(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};

    // BGRA → RGB24
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
    for (int y = 0; y < h; ++y) {
        const auto* src = static_cast<const uint8_t*>(mapped.pData)
                          + static_cast<size_t>(y) * mapped.RowPitch;
        auto* dst = rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3u;
        for (int x = 0; x < w; ++x) {
            dst[x*3+0] = src[x*4+2]; // R
            dst[x*3+1] = src[x*4+1]; // G
            dst[x*3+2] = src[x*4+0]; // B
        }
    }
    m_context->Unmap(m_staging.Get(), 0);

    out_w = w; out_h = h;
    out_left = clipped.left; out_top = clipped.top;
    return rgb;
}

void DxgiCaptureBackend::Impl::end() {
    if (m_duplication && m_last_frame) m_duplication->ReleaseFrame();
    m_last_frame.Reset();
}

void DxgiCaptureBackend::Impl::note_failure(bool& out_should_reset) {
    out_should_reset = false;
    if (++m_fail_streak >= kFailResetThreshold) {
        m_fail_streak    = 0;
        out_should_reset = true;
    }
}

void DxgiCaptureBackend::Impl::note_success()   { m_fail_streak = 0; }
void DxgiCaptureBackend::Impl::reset_counters() { m_fail_streak = 0; }
void DxgiCaptureBackend::Impl::full_reset()     { release_duplication(); init_output(m_output_index); }

// ============================================================
// DxgiCaptureBackend 公开接口
// ============================================================

/*static*/
bool DxgiCaptureBackend::probe() { return Impl{}.is_available(); }

DxgiCaptureBackend::DxgiCaptureBackend()  : m_impl(std::make_unique<Impl>()) {}
DxgiCaptureBackend::~DxgiCaptureBackend() = default;

void DxgiCaptureBackend::on_new_session() {
    if (m_impl) m_impl->reset_counters();
}

bool DxgiCaptureBackend::capture_tiles(
    std::span<const win32::WindowInfo> surfaces,
    std::vector<WindowTile>&           out_tiles,
    uint64_t /*now_unix_ms*/)
{
    out_tiles.clear();
    if (!m_impl || !m_impl->is_available() || surfaces.empty()) {
        std::cout << "[dxgi] unavailable or no surfaces\n";
        return false;
    }

    std::vector<HWND> hwnds;
    hwnds.reserve(surfaces.size());
    for (const auto& s : surfaces) hwnds.push_back(s.hwnd);

    if (!m_impl->begin(hwnds)) {
        bool should_reset = false;
        if (!m_impl->last_timed_out())
            m_impl->note_failure(should_reset);
        if (should_reset) m_impl->full_reset();
        std::cout << "[dxgi] begin failed timed_out=" << m_impl->last_timed_out() << '\n';
        return false;
    }

    for (const auto& s : surfaces) {
        WindowTile t{};
        t.hwnd        = s.hwnd;
        t.rect_screen = s.rect_screen;
        t.z_order     = s.z_order;
        t.rgb = m_impl->copy_window_rgb(s.hwnd, t.w, t.h, t.origin_left, t.origin_top);

        const size_t expected = static_cast<size_t>(t.w) * static_cast<size_t>(t.h) * 3u;
        if (t.rgb.empty() || t.w <= 0 || t.h <= 0 || t.rgb.size() != expected) {
            m_impl->end();
            bool should_reset = false;
            m_impl->note_failure(should_reset);
            if (should_reset) m_impl->full_reset();
            std::cout << "[dxgi] copy failed hwnd=" << static_cast<void*>(s.hwnd) << '\n';
            return false;
        }
        out_tiles.push_back(std::move(t));
    }

    m_impl->end();
    m_impl->note_success();
    return true;
}

// begin 辅助：选输出器 + acquire
bool DxgiCaptureBackend::Impl::begin(const std::vector<HWND>& hwnds) {
    if (!m_available || hwnds.empty()) return false;
    win32::Window wops;
    for (HWND h : hwnds)
        if (!wops.is_valid(h)) return false;
    if (!ensure_output_for_window(hwnds[0])) return false;
    // 验证其余窗口也在同一输出器
    for (size_t i = 1; i < hwnds.size(); ++i) {
        RECT wr{};
        if (!wops.get_effective_rect(hwnds[i], wr)) return false;
        const LONG cx = (wr.left + wr.right)  / 2;
        const LONG cy = (wr.top  + wr.bottom) / 2;
        if (cx < m_output_rect.left || cx >= m_output_rect.right
         || cy < m_output_rect.top  || cy >= m_output_rect.bottom)
            return false;
    }
    return acquire_frame();
}

} // namespace capture

#else // !_WIN32

namespace capture {
bool DxgiCaptureBackend::probe() { return false; }
DxgiCaptureBackend::DxgiCaptureBackend()  : m_impl(nullptr) {}
DxgiCaptureBackend::~DxgiCaptureBackend() = default;
void DxgiCaptureBackend::on_new_session() {}
bool DxgiCaptureBackend::capture_tiles(std::span<const win32::WindowInfo>,
                                        std::vector<WindowTile>&, uint64_t)
{ return false; }
} // namespace capture

#endif
