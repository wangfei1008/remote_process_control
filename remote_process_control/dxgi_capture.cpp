#include "dxgi_capture.h"

#include <algorithm>
#include <cstring>

DXGICapture::DXGICapture()
{
    m_available = init();
}

DXGICapture::~DXGICapture()
{
    reset_duplication();
}

bool DXGICapture::init()
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

bool DXGICapture::init_output_by_index(UINT outputIndex)
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

void DXGICapture::reset_duplication()
{
    if (m_duplication) {
        m_duplication->ReleaseFrame();
    }
    m_duplication.Reset();
    m_last_desktop_frame.Reset();
}

bool DXGICapture::ensure_output_for_window(HWND hwnd)
{
    if (!hwnd) return false;
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
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

bool DXGICapture::ensure_staging_texture(int width, int height)
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

bool DXGICapture::acquire_desktop_frame()
{
    if (!m_duplication) return false;
    if (m_last_desktop_frame) {
        m_last_desktop_frame.Reset();
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
    HRESULT hr = m_duplication->AcquireNextFrame(16, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        if (!init_output_by_index(m_output_index)) return false;
        hr = m_duplication->AcquireNextFrame(16, &frameInfo, &desktopResource);
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

std::vector<uint8_t> DXGICapture::capture_window_rgb(HWND hwnd, int& outWidth, int& outHeight, int& outLeft, int& outTop)
{
    outWidth = 0;
    outHeight = 0;
    outLeft = 0;
    outTop = 0;
    if (!m_available || !hwnd || !IsWindow(hwnd)) return {};
    if (!ensure_output_for_window(hwnd)) return {};

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return {};
    RECT clipped = wr;
    clipped.left = std::max(clipped.left, m_output_rect.left);
    clipped.top = std::max(clipped.top, m_output_rect.top);
    clipped.right = std::min(clipped.right, m_output_rect.right);
    clipped.bottom = std::min(clipped.bottom, m_output_rect.bottom);
    int w = clipped.right - clipped.left;
    int h = clipped.bottom - clipped.top;
    if (w <= 1 || h <= 1) return {};

    w &= ~1;
    h &= ~1;
    if (w <= 0 || h <= 0) return {};

    if (!acquire_desktop_frame()) return {};
    if (!ensure_staging_texture(w, h)) {
        m_duplication->ReleaseFrame();
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
        m_duplication->ReleaseFrame();
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
    m_duplication->ReleaseFrame();

    outWidth = w;
    outHeight = h;
    outLeft = clipped.left;
    outTop = clipped.top;
    return rgb;
}

