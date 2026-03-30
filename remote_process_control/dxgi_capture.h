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
    // Recreate/reset internal duplication resources after repeated capture failures.
    void reset();
    std::vector<uint8_t> capture_window_rgb(HWND hwnd, int& outWidth, int& outHeight, int& outLeft, int& outTop);

private:
    bool init();
    bool init_output_by_index(UINT outputIndex);
    bool ensure_output_for_window(HWND hwnd);
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

