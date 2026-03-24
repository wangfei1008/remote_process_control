#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // For SendInput
#include <cstdint>

class InputController 
{
public:
	/** Enable process DPI awareness so geometry and cursor mapping stay consistent. */
	static void ensure_process_dpi_awareness();
    static void note_input_activity();
    static uint64_t last_input_activity_ms();
    static InputController* instance();
    void simulate_mouse_move(int x, int y);
	void simulate_mouse_move(int x, int y, int abs_x, int abs_y, int video_width, int video_height);
    /** Set the primary window target used by mouse-focus and coordinate mapping. */
    void set_mouse_target(HWND hwnd);
    /** Set the merged capture rectangle produced by ProcessManager::capture_all_windows_image. */
    void set_capture_screen_rect(int left, int top, int width, int height);
    void clear_mouse_target();
	// absX, absY: absolute coordinates in the streamed video space.
	// videoWidth, videoHeight: current streamed video dimensions.
    void simulate_mouse_down(int button, int x, int y); // button: 0=left, 1=right, 2=middle
    void simulate_mouse_up(int button, int x, int y);
	void simulate_mouse_double_click(int button, int x, int y);
	void simulate_mouse_wheel(int delta_x, int delta_y, int x, int y); // delta: positive=up, negative=down
    void simulate_key_press(int keyCode); // Virtual key code, e.g., VK_RETURN, 'A'
	void simulate_key_down(int key, int code, int key_code, int shift_key, int ctrl_key, int alt_key, int meta_key);
	void simulate_key_up(int key, int code, int key_code, int shift_key, int ctrl_key, int alt_key, int meta_key);
    void simulate_key_release(int keyCode);
private:
	void move_mouse_to_screen_pixel(int screen_x, int screen_y);
	/** Bring the target window to foreground before injecting click/keyboard events. */
	void bring_mouse_target_foreground();
	static InputController* m_instance;
	HWND m_map_hwnd = nullptr;
	int m_cap_left = 0;
	int m_cap_top = 0;
	int m_cap_w = 0;
	int m_cap_h = 0;
};

