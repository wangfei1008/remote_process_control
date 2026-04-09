#include "input_controller.h"

#include "common/window_rect_utils.h"

#include <iostream>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <atomic>
#include <chrono>
#include <mutex>

#ifndef ASFW_ANY
#define ASFW_ANY 0x0000FFFF
#endif

#ifndef MOUSEEVENTF_VIRTUALDESK
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#endif

input_controller* input_controller::m_instance = nullptr;
static std::atomic<uint64_t> g_last_input_activity_ms{0};

static DWORD vk_extended_flag(WORD vk)
{
	switch (vk) {
	case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
	case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
	case VK_INSERT: case VK_DELETE:
	case VK_NUMLOCK:
	case VK_RCONTROL: case VK_RMENU:
	case VK_APPS: case VK_RWIN:
		return KEYEVENTF_EXTENDEDKEY;
	default:
		return 0;
	}
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          进程级输入环境初始化（当前为启用 DPI 感知，保证坐标与采集一致）
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void input_controller::ensure_process_dpi_awareness()
{
	static bool done = false;
	if (done) return;
	done = true;
	// 在 Win10 1703+ 下，PER_MONITOR_AWARE_V2 可对齐窗口矩形/采集/SetCursorPos。
	using SetCtxFn = BOOL(WINAPI*)(HANDLE);
	using DpiCtx = HANDLE;
	const DpiCtx PER_MONITOR_AWARE_V2 = reinterpret_cast<DpiCtx>(static_cast<LONG_PTR>(-4));
	SetCtxFn fn = reinterpret_cast<SetCtxFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
	if (fn)
		(void)fn(PER_MONITOR_AWARE_V2);
}

void input_controller::note_input_activity()
{
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    g_last_input_activity_ms.store(now, std::memory_order_relaxed);
}

uint64_t input_controller::last_input_activity_ms()
{
    return g_last_input_activity_ms.load(std::memory_order_relaxed);
}

input_controller* input_controller::instance()
{
	if (!m_instance) {
		m_instance = new input_controller();
	}
	return m_instance;
}

void input_controller::bind_mouse_target(HWND hwnd)
{
	m_map_hwnd = hwnd;
}

void input_controller::unbind_mouse_target()
{
	m_map_hwnd = nullptr;
	m_cap_left = m_cap_top = m_cap_w = m_cap_h = 0;
}

void input_controller::set_capture_screen_rect(int left, int top, int width, int height)
{
	m_cap_left = left;
	m_cap_top = top;
	m_cap_w = width;
	m_cap_h = height;
}

void input_controller::bring_mouse_target_foreground()
{
	if (!m_map_hwnd || !IsWindow(m_map_hwnd)) return;
	if (GetForegroundWindow() == m_map_hwnd) return;

	(void)AllowSetForegroundWindow(ASFW_ANY);
	ShowWindow(m_map_hwnd, SW_RESTORE);

	HWND fg = GetForegroundWindow();
	const DWORD curTid = GetCurrentThreadId();
	const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;

	DWORD lockTimeout = 0;
	SystemParametersInfo(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &lockTimeout, 0);
	SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)0, SPIF_SENDCHANGE);

	if (fgTid && fgTid != curTid) {
		AttachThreadInput(fgTid, curTid, TRUE);
	}

	SetForegroundWindow(m_map_hwnd);
	BringWindowToTop(m_map_hwnd);

	if (fgTid && fgTid != curTid) {
		AttachThreadInput(fgTid, curTid, FALSE);
	}

	SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)(UINT_PTR)lockTimeout, SPIF_SENDCHANGE);
}

void input_controller::move_mouse_to_screen_pixel(int screen_x, int screen_y)
{
	// 坐标映射依赖采集矩形、窗口矩形与进程 DPI 感知保持一致。
	if (::SetCursorPos(screen_x, screen_y))
		return;

	INPUT input{};
	input.type = INPUT_MOUSE;
	const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	if (vw <= 0 || vh <= 0) {
		const int cx = GetSystemMetrics(SM_CXSCREEN);
		const int cy = GetSystemMetrics(SM_CYSCREEN);
		input.mi.dx = (LONG)((static_cast<int64_t>(screen_x) * 65535) / std::max(1, cx));
		input.mi.dy = (LONG)((static_cast<int64_t>(screen_y) * 65535) / std::max(1, cy));
		input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
	} else {
		const int64_t nx = (static_cast<int64_t>(screen_x - vx) * 65535) / std::max(1, vw);
		const int64_t ny = (static_cast<int64_t>(screen_y - vy) * 65535) / std::max(1, vh);
		input.mi.dx = (LONG)std::min<int64_t>(65535, std::max<int64_t>(0, nx));
		input.mi.dy = (LONG)std::min<int64_t>(65535, std::max<int64_t>(0, ny));
		input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
	}
	SendInput(1, &input, sizeof(INPUT));
}

void input_controller::simulate_mouse_move(int x, int y)
{
	note_input_activity();
	move_mouse_to_screen_pixel(x, y);
}

void input_controller::simulate_mouse_move(int x, int y, int abs_x, int abs_y, int video_width, int video_height)
{
	note_input_activity();
	// 优先使用合并采集矩形，否则退回主窗口 GetWindowRect
	if (video_width > 0 && video_height > 0 && m_cap_w > 0 && m_cap_h > 0) {
		// 使用像素中心归一化以匹配前端映射
		const double fx = std::max(0.0, std::min(1.0,
			(static_cast<double>(abs_x) + 0.5) / static_cast<double>(video_width)));
		const double fy = std::max(0.0, std::min(1.0,
			(static_cast<double>(abs_y) + 0.5) / static_cast<double>(video_height)));
		const int ox = static_cast<int>(std::floor(fx * static_cast<double>(m_cap_w)));
		const int oy = static_cast<int>(std::floor(fy * static_cast<double>(m_cap_h)));
		const int screen_x = m_cap_left + std::max(0, std::min(m_cap_w - 1, ox));
		const int screen_y = m_cap_top + std::max(0, std::min(m_cap_h - 1, oy));
		move_mouse_to_screen_pixel(screen_x, screen_y);
		return;
	}
	if (m_map_hwnd && IsWindow(m_map_hwnd) && video_width > 0 && video_height > 0) {
		RECT wr{};
		if (window_rect_utils::get_effective_window_rect(m_map_hwnd, wr)) {
			const int win_w = wr.right - wr.left;
			const int win_h = wr.bottom - wr.top;
			if (win_w > 0 && win_h > 0) {
				const double fx = std::max(0.0, std::min(1.0,
					(static_cast<double>(abs_x) + 0.5) / static_cast<double>(video_width)));
				const double fy = std::max(0.0, std::min(1.0,
					(static_cast<double>(abs_y) + 0.5) / static_cast<double>(video_height)));
				const int ox = static_cast<int>(std::floor(fx * static_cast<double>(win_w)));
				const int oy = static_cast<int>(std::floor(fy * static_cast<double>(win_h)));
				const int screen_x = wr.left + std::max(0, std::min(win_w - 1, ox));
				const int screen_y = wr.top + std::max(0, std::min(win_h - 1, oy));
				move_mouse_to_screen_pixel(screen_x, screen_y);
				return;
			}
		}
	}
	if (video_width > 0 && video_height > 0) {
		const int cx = GetSystemMetrics(SM_CXSCREEN);
		const int cy = GetSystemMetrics(SM_CYSCREEN);
		const int screen_x = (abs_x * cx) / video_width;
		const int screen_y = (abs_y * cy) / video_height;
		move_mouse_to_screen_pixel(screen_x, screen_y);
	} else {
		INPUT input{};
		input.type = INPUT_MOUSE;
		input.mi.dx = (LONG)x;
		input.mi.dy = (LONG)y;
		input.mi.dwFlags = MOUSEEVENTF_MOVE;
		SendInput(1, &input, sizeof(INPUT));
	}
}

void input_controller::simulate_mouse_down(int button, int x, int y)
{
	note_input_activity();
	bring_mouse_target_foreground();

	INPUT input{};
	input.type = INPUT_MOUSE;
	input.mi.dx = (LONG)x;
	input.mi.dy = (LONG)y;
	input.mi.dwFlags = 0;

	switch (button) {
	case 0: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
	case 1: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
	case 2: input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
	default: return;
	}
	SendInput(1, &input, sizeof(INPUT));
}

void input_controller::simulate_mouse_up(int button, int x, int y)
{
	note_input_activity();
	bring_mouse_target_foreground();

	INPUT input{};
	input.type = INPUT_MOUSE;
	input.mi.dx = (LONG)x;
	input.mi.dy = (LONG)y;
	input.mi.dwFlags = 0;

	switch (button) {
	case 0: input.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
	case 1: input.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
	case 2: input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
	default: return;
	}
	SendInput(1, &input, sizeof(INPUT));
}

void input_controller::simulate_mouse_double_click(int button, int x, int y)
{
	note_input_activity();
	simulate_mouse_down(button, x, y);
	simulate_mouse_up(button, x, y);
	Sleep(50); // 点击之间的短延时
	simulate_mouse_down(button, x, y);
	simulate_mouse_up(button, x, y);
}

void input_controller::simulate_mouse_wheel(int delta_x, int delta_y, int x, int y)
{
	note_input_activity();
	bring_mouse_target_foreground();

	// 将前端滚轮增量缩放到 Windows WHEEL_DELTA 单位（120）。
	const int WHEEL = static_cast<int>(WHEEL_DELTA);
	auto scaleWheel = [WHEEL](int d) -> int {
		if (d == 0) return 0;
		const int sign = (d > 0) ? 1 : -1;
		const int mag = std::max(WHEEL, (std::abs(d) * WHEEL) / 100);
		return sign * mag;
	};

	INPUT input{};
	input.type = INPUT_MOUSE;
	input.mi.dx = (LONG)x;
	input.mi.dy = (LONG)y;
	input.mi.dwFlags = MOUSEEVENTF_WHEEL;
	input.mi.mouseData = static_cast<DWORD>(scaleWheel(delta_y));
	if (delta_x != 0) {
		INPUT inputX{};
		inputX.type = INPUT_MOUSE;
		inputX.mi.dx = (LONG)x;
		inputX.mi.dy = (LONG)y;
		inputX.mi.dwFlags = MOUSEEVENTF_HWHEEL;
		inputX.mi.mouseData = static_cast<DWORD>(scaleWheel(delta_x));
		SendInput(1, &inputX, sizeof(INPUT));
	}
	if (delta_y != 0) {
		SendInput(1, &input, sizeof(INPUT));
	}
}



void input_controller::simulate_key_press(int key_code)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = 0;
    input.ki.time = 0;
    input.ki.dwExtraInfo = 0;
    input.ki.wVk = static_cast<WORD>(key_code);
    input.ki.dwFlags = vk_extended_flag(static_cast<WORD>(key_code));
    SendInput(1, &input, sizeof(INPUT));
}

void input_controller::simulate_key_down(int key, int code, int key_code, int shift_key, int ctrl_key, int alt_key, int meta_key)
{
	note_input_activity();
	if (key_code == 0) return;
	bring_mouse_target_foreground();
    if (shift_key) simulate_key_press(VK_SHIFT); 
    if (ctrl_key)  simulate_key_press(VK_CONTROL); 
    if (alt_key)   simulate_key_press(VK_MENU);
	// 需要时将前端 Meta 映射为 Windows 键（LWIN）。
	if (meta_key && key_code != VK_LWIN && key_code != VK_RWIN) simulate_key_press(VK_LWIN);
    simulate_key_press(key_code);
}

void input_controller::simulate_key_up(int key, int code, int key_code, int shift_key, int ctrl_key, int alt_key, int meta_key)
{
	note_input_activity();
	if (key_code == 0) return;
	bring_mouse_target_foreground();
    simulate_key_release(key_code);
    if (meta_key && key_code != VK_LWIN && key_code != VK_RWIN) simulate_key_release(VK_LWIN);
    if (alt_key)   simulate_key_release(VK_MENU);
    if (ctrl_key)  simulate_key_release(VK_CONTROL);
    if (shift_key) simulate_key_release(VK_SHIFT);
}

void input_controller::simulate_key_release(int key_code)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = 0;
    input.ki.time = 0;
    input.ki.dwExtraInfo = 0;
    input.ki.wVk = static_cast<WORD>(key_code);
    input.ki.dwFlags = KEYEVENTF_KEYUP | vk_extended_flag(static_cast<WORD>(key_code));
    SendInput(1, &input, sizeof(INPUT));
}
