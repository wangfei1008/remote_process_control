#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // 用于 SendInput
#include <cstdint>
#include <mutex>

class InputController 
{
public:
	// 启用进程 DPI 感知，保持几何与光标映射一致。
	static void ensure_process_dpi_awareness();
    static void note_input_activity();
    static uint64_t last_input_activity_ms();
    static InputController* instance();
    void simulate_mouse_move(int x, int y);
	void simulate_mouse_move(int x, int y, int abs_x, int abs_y, int video_width, int video_height);
    // 绑定主窗口目标；返回 bind_id，解绑时传入同一 id（仅当仍为当前有效绑定时才会清除）。
    void bind_mouse_target(HWND hwnd);
    void unbind_mouse_target();
    //设置远程进程采集管线输出的合并采集矩形。
    void set_capture_screen_rect(int left, int top, int width, int height);

	// 参数 absX、absY：流媒体视频空间中的绝对坐标。
	// 参数 videoWidth、videoHeight：当前视频流分辨率。
    void simulate_mouse_down(int button, int x, int y); // button：0=左键，1=右键，2=中键
    void simulate_mouse_up(int button, int x, int y);
	void simulate_mouse_double_click(int button, int x, int y);
	void simulate_mouse_wheel(int delta_x, int delta_y, int x, int y); // delta：正值上滚，负值下滚
    void simulate_key_press(int keyCode); // 虚拟键码，例如 VK_RETURN、'A'
	void simulate_key_down(int key, int code, int key_code, int shift_key, int ctrl_key, int alt_key, int meta_key);
	void simulate_key_up(int key, int code, int key_code, int shift_key, int ctrl_key, int alt_key, int meta_key);
    void simulate_key_release(int keyCode);
private:
	void move_mouse_to_screen_pixel(int screen_x, int screen_y);
	// 注入点击/键盘事件前将目标窗口置前。
	void bring_mouse_target_foreground();
	static InputController* m_instance;
	HWND m_map_hwnd = nullptr;
	int m_cap_left = 0;
	int m_cap_top = 0;
	int m_cap_w = 0;
	int m_cap_h = 0;
};

