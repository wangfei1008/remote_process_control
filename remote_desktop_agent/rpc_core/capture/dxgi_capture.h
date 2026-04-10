#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>

class DXGICapture
{
public:
    DXGICapture();
    ~DXGICapture();

    bool is_available() const { return m_available; }
    // 多次采集失败后重建/重置内部复制资源。
    void reset();
    std::vector<uint8_t> capture_window_rgb(HWND hwnd, int& outWidth, int& outHeight, int& outLeft, int& outTop);

    // 单帧桌面复制：一次 AcquireNextFrame，再从同一帧裁剪多个窗口（多显示器下要求所有窗口中心落在当前 output 内）。
    bool begin_multiwindow_desktop_capture(const std::vector<HWND>& hwnds);
    std::vector<uint8_t> copy_acquired_window_to_rgb(HWND hwnd, int& outWidth, int& outHeight, int& outLeft, int& outTop);
    void end_desktop_capture();

private:
    bool init();
    bool init_output_by_index(UINT outputIndex);
    bool ensure_output_for_window(HWND hwnd);
    bool window_center_on_current_output(HWND hwnd) const;
    bool ensure_staging_texture(int width, int height);
    bool acquire_desktop_frame();
    void reset_duplication();

private:
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
};

