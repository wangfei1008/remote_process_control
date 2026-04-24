#include "capture/dxgi_process_ui_capture_backend.h"

#include "common/window_ops.h"
#include "app/runtime_config.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <iostream>

struct DxgiProcessUiCaptureBackend::Impl {
    Impl();
    ~Impl();

    bool is_available() const { return m_available; }
    void reset();
    bool begin_multiwindow_desktop_capture(const std::vector<HWND>& hwnds);
    std::vector<uint8_t> copy_acquired_window_to_rgb(HWND hwnd, int& outWidth, int& outHeight, int& outLeft, int& outTop);
    void end_desktop_capture();
    void note_acquisition_failure(bool& should_reset_duplication);
    void note_acquisition_success();
    void reset_session_recovery();
    bool last_acquire_timed_out() const { return m_last_acquire_timed_out; }

private:
    bool init();
    bool init_output_by_index(UINT outputIndex);
    bool ensure_output_for_window(HWND hwnd);
    bool window_center_on_current_output(HWND hwnd) const;
    bool ensure_staging_texture(int width, int height);
    bool acquire_desktop_frame();
    void reset_duplication();

    bool m_available = false;
    UINT m_output_index = 0;
    int m_output_width = 0;
    int m_output_height = 0;
    RECT m_output_rect{ 0, 0, 0, 0 };

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_last_desktop_frame;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;

    bool m_last_acquire_timed_out = false;
    int m_acquisition_fail_streak = 0;
    static constexpr int k_acquisition_fail_reset_threshold = 6;
};

DxgiProcessUiCaptureBackend::Impl::Impl()
{
    m_available = init();
}

DxgiProcessUiCaptureBackend::Impl::~Impl()
{
    reset_duplication();
}

void DxgiProcessUiCaptureBackend::Impl::reset()
{
    reset_duplication();
}

bool DxgiProcessUiCaptureBackend::Impl::init()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#endif
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL outLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &m_device,
        &outLevel,
        &m_context);
    if (FAILED(hr) || !m_device || !m_context) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) return false;

    return init_output_by_index(0);
}

bool DxgiProcessUiCaptureBackend::Impl::init_output_by_index(UINT outputIndex)
{
    if (!m_device || !m_context) return false;
    reset_duplication();

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(outputIndex, &output);
    if (FAILED(hr) || !output) return false;

    DXGI_OUTPUT_DESC outDesc{};
    output->GetDesc(&outDesc);
    m_output_rect = outDesc.DesktopCoordinates;
    m_output_width = m_output_rect.right - m_output_rect.left;
    m_output_height = m_output_rect.bottom - m_output_rect.top;
    if (m_output_width <= 0 || m_output_height <= 0) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr) || !output1) return false;

    hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr) || !m_duplication) return false;

    m_output_index = outputIndex;
    m_staging.Reset();
    return true;
}

void DxgiProcessUiCaptureBackend::Impl::reset_duplication()
{
    if (m_duplication) {
        m_duplication->ReleaseFrame();
    }
    m_duplication.Reset();
    m_last_desktop_frame.Reset();
}

bool DxgiProcessUiCaptureBackend::Impl::ensure_output_for_window(HWND hwnd)
{
    if (!hwnd) return false;
    RECT wr{};
    window_ops wops;
    if (!wops.get_effective_window_rect(hwnd, wr)) return false;
    const LONG cx = (wr.left + wr.right) / 2;
    const LONG cy = (wr.top + wr.bottom) / 2;
    if (cx >= m_output_rect.left && cx < m_output_rect.right &&
        cy >= m_output_rect.top && cy < m_output_rect.bottom &&
        m_duplication) {
        return true;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) return false;

    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
        if (!output) continue;
        DXGI_OUTPUT_DESC d{};
        output->GetDesc(&d);
        if (cx >= d.DesktopCoordinates.left && cx < d.DesktopCoordinates.right &&
            cy >= d.DesktopCoordinates.top && cy < d.DesktopCoordinates.bottom) {
            return init_output_by_index(i);
        }
    }
    return init_output_by_index(0);
}

bool DxgiProcessUiCaptureBackend::Impl::ensure_staging_texture(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    if (m_staging) {
        D3D11_TEXTURE2D_DESC desc{};
        m_staging->GetDesc(&desc);
        if (static_cast<int>(desc.Width) == width && static_cast<int>(desc.Height) == height) {
            return true;
        }
        m_staging.Reset();
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_staging);
    return SUCCEEDED(hr) && m_staging;
}

bool DxgiProcessUiCaptureBackend::Impl::acquire_desktop_frame()
{
    if (!m_duplication) return false;
    m_last_acquire_timed_out = false;
    if (m_last_desktop_frame) {
        m_last_desktop_frame.Reset();
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
    const int timeout_ms = (std::max)(0, runtime_config::get_int("RPC_DXGI_ACQUIRE_TIMEOUT_MS", 1));
    HRESULT hr = m_duplication->AcquireNextFrame(static_cast<UINT>(timeout_ms), &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        m_last_acquire_timed_out = true;
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        if (!init_output_by_index(m_output_index)) return false;
        hr = m_duplication->AcquireNextFrame(static_cast<UINT>(timeout_ms), &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            m_last_acquire_timed_out = true;
            return false;
        }
    }
    if (FAILED(hr) || !desktopResource) return false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    if (FAILED(hr) || !desktopTexture) {
        m_duplication->ReleaseFrame();
        return false;
    }
    m_last_desktop_frame = desktopTexture;
    return true;
}

bool DxgiProcessUiCaptureBackend::Impl::window_center_on_current_output(HWND hwnd) const
{
    window_ops wops;
    if (!wops.is_valid(hwnd)) return false;
    RECT wr{};
    if (!wops.get_effective_window_rect(hwnd, wr)) return false;
    const LONG cx = (wr.left + wr.right) / 2;
    const LONG cy = (wr.top + wr.bottom) / 2;
    return cx >= m_output_rect.left && cx < m_output_rect.right && cy >= m_output_rect.top && cy < m_output_rect.bottom;
}

bool DxgiProcessUiCaptureBackend::Impl::begin_multiwindow_desktop_capture(const std::vector<HWND>& hwnds)
{
    if (!m_available || hwnds.empty()) return false;
    window_ops wops;
    for (HWND h : hwnds) {
        if (!wops.is_valid(h)) return false;
    }
    if (!ensure_output_for_window(hwnds[0])) return false;
    for (size_t i = 1; i < hwnds.size(); ++i) {
        if (!window_center_on_current_output(hwnds[i])) return false;
    }
    return acquire_desktop_frame();
}

std::vector<uint8_t> DxgiProcessUiCaptureBackend::Impl::copy_acquired_window_to_rgb(HWND hwnd,
                                                                                     int& outWidth,
                                                                                     int& outHeight,
                                                                                     int& outLeft,
                                                                                     int& outTop)
{
    outWidth = 0;
    outHeight = 0;
    outLeft = 0;
    outTop = 0;
    window_ops wops;
    if (!m_available || !wops.is_valid(hwnd) || !m_last_desktop_frame) return {};

    RECT winRc{};
    if (!wops.get_effective_window_rect(hwnd, winRc)) return {};

    RECT clipped{};
    clipped.left = winRc.left;
    clipped.top = winRc.top;
    clipped.right = winRc.right;
    clipped.bottom = winRc.bottom;
    clipped.left = (std::max)(clipped.left, m_output_rect.left);
    clipped.top = (std::max)(clipped.top, m_output_rect.top);
    clipped.right = (std::min)(clipped.right, m_output_rect.right);
    clipped.bottom = (std::min)(clipped.bottom, m_output_rect.bottom);
    int w = clipped.right - clipped.left;
    int h = clipped.bottom - clipped.top;
    if (w <= 1 || h <= 1) return {};

    w &= ~1;
    h &= ~1;
    if (w <= 0 || h <= 0) return {};

    if (!ensure_staging_texture(w, h)) {
        return {};
    }

    D3D11_BOX srcBox{};
    srcBox.left = static_cast<UINT>(clipped.left - m_output_rect.left);
    srcBox.top = static_cast<UINT>(clipped.top - m_output_rect.top);
    srcBox.right = static_cast<UINT>(srcBox.left + w);
    srcBox.bottom = static_cast<UINT>(srcBox.top + h);
    srcBox.front = 0;
    srcBox.back = 1;

    m_context->CopySubresourceRegion(m_staging.Get(), 0, 0, 0, 0, m_last_desktop_frame.Get(), 0, &srcBox);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return {};
    }

    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dst = rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3u;
        for (int x = 0; x < w; ++x) {
            const uint8_t b = src[x * 4 + 0];
            const uint8_t g = src[x * 4 + 1];
            const uint8_t r = src[x * 4 + 2];
            dst[x * 3 + 0] = r;
            dst[x * 3 + 1] = g;
            dst[x * 3 + 2] = b;
        }
    }

    m_context->Unmap(m_staging.Get(), 0);

    outWidth = w;
    outHeight = h;
    outLeft = clipped.left;
    outTop = clipped.top;
    return rgb;
}

void DxgiProcessUiCaptureBackend::Impl::end_desktop_capture()
{
    if (m_duplication && m_last_desktop_frame) {
        m_duplication->ReleaseFrame();
    }
    m_last_desktop_frame.Reset();
}

void DxgiProcessUiCaptureBackend::Impl::note_acquisition_failure(bool& should_reset_duplication)
{
    should_reset_duplication = false;
    ++m_acquisition_fail_streak;
    if (m_acquisition_fail_streak >= k_acquisition_fail_reset_threshold) {
        m_acquisition_fail_streak = 0;
        should_reset_duplication = true;
    }
}

void DxgiProcessUiCaptureBackend::Impl::note_acquisition_success()
{
    m_acquisition_fail_streak = 0;
}

void DxgiProcessUiCaptureBackend::Impl::reset_session_recovery()
{
    m_acquisition_fail_streak = 0;
}

bool DxgiProcessUiCaptureBackend::probe()
{
    Impl probe;
    return probe.is_available();
}

DxgiProcessUiCaptureBackend::DxgiProcessUiCaptureBackend()
    : m_impl(std::make_unique<Impl>())
{
}

DxgiProcessUiCaptureBackend::~DxgiProcessUiCaptureBackend() = default;

bool DxgiProcessUiCaptureBackend::capture_tiles(const std::vector<window_ops::window_info>& surfaces, std::vector<ProcessUiWindowTile>& tiles, uint64_t /*now_unix_ms*/)
{
    tiles.clear();
    if (!m_impl || !m_impl->is_available() || surfaces.empty()) {
		std::cout << "[dxgi capture] unavailable or no surfaces\n";
        return false;
    }        

    std::vector<HWND> hwnds;
    hwnds.reserve(surfaces.size());
    for (const auto& s : surfaces) hwnds.push_back(s.hwnd);

    if (!m_impl->begin_multiwindow_desktop_capture(hwnds)) {
        bool should_reset = false;
        // Non-blocking AcquireNextFrame can legitimately time out when the desktop has no new frame.
        // Do NOT treat WAIT_TIMEOUT as a failure streak that triggers duplication reset.
        if (!m_impl->last_acquire_timed_out()) {
            m_impl->note_acquisition_failure(should_reset);
        }
        if (should_reset) m_impl->reset();
		std::cout << "[dxgi capture] begin_multiwindow_desktop_capture failed; timed_out=" << m_impl->last_acquire_timed_out() << "\n";
        return false;
    }

    for (const auto& s : surfaces) {
        ProcessUiWindowTile t{};
        t.hwnd = s.hwnd;
        t.rect_screen = s.rect_screen;
        t.z_order = s.z_order;
        t.rgb = m_impl->copy_acquired_window_to_rgb(s.hwnd, t.w, t.h, t.origin_left, t.origin_top);
        const size_t expected = static_cast<size_t>(t.w) * static_cast<size_t>(t.h) * 3u;
        if (t.rgb.empty() || t.w <= 0 || t.h <= 0 || t.rgb.size() != expected) {
            m_impl->end_desktop_capture();
            bool should_reset = false;
            m_impl->note_acquisition_failure(should_reset);
            if (should_reset) m_impl->reset();
            std::cout << "[dxgi capture] copy_acquired_window_to_rgb failed for hwnd=" << static_cast<void*>(s.hwnd)
                      << " expected_rgb_size=" << expected
                      << " got_rgb_size=" << t.rgb.size()
                      << " w=" << t.w
                      << " h=" << t.h
				<< "\n";
            return false;
        }
        tiles.push_back(std::move(t));
    }
    m_impl->end_desktop_capture();
    m_impl->note_acquisition_success();
    return true;
}

void DxgiProcessUiCaptureBackend::reset_session_recovery()
{
    if (m_impl) m_impl->reset_session_recovery();
}
