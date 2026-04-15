#include "rdr_input.hpp"

#include "receiver_context.hpp"
#include "rdr_d3d.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

using json = nlohmann::json;

static bool IsInsideVideoRect(int x, int y) {
	const int bx = g_lbX.load(std::memory_order_relaxed);
	const int by = g_lbY.load(std::memory_order_relaxed);
	const int bw = g_lbW.load(std::memory_order_relaxed);
	const int bh = g_lbH.load(std::memory_order_relaxed);
	if (bw <= 0 || bh <= 0) return false;
	return x >= bx && x < (bx + bw) && y >= by && y < (by + bh);
}

static void MapClientToVideo(int cx, int cy, int& outAbsX, int& outAbsY) {
	const int vw = g_videoW.load(std::memory_order_relaxed);
	const int vh = g_videoH.load(std::memory_order_relaxed);
	const int bx = g_lbX.load(std::memory_order_relaxed);
	const int by = g_lbY.load(std::memory_order_relaxed);
	const int bw = g_lbW.load(std::memory_order_relaxed);
	const int bh = g_lbH.load(std::memory_order_relaxed);
	if (vw <= 0 || vh <= 0 || bw <= 0 || bh <= 0) {
		outAbsX = 0;
		outAbsY = 0;
		return;
	}

	const double fx = (double)(cx - bx) / (double)bw;
	const double fy = (double)(cy - by) / (double)bh;
	const int ax = (int)std::round(fx * (double)vw);
	const int ay = (int)std::round(fy * (double)vh);
	outAbsX = (std::max)(0, (std::min)(vw - 1, ax));
	outAbsY = (std::max)(0, (std::min)(vh - 1, ay));
}

LRESULT CALLBACK RdrInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_RDR_NEW_VIDEO_FRAME:
		return 0;
	case WM_DESTROY:
		g_exitRequested.store(true, std::memory_order_relaxed);
		PostQuitMessage(0);
		return 0;
	case WM_KEYDOWN: {
		if ((int)wParam == VK_ESCAPE) {
			g_exitRequested.store(true, std::memory_order_relaxed);
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return 0;
		}
		if (!g_inputArmed.load(std::memory_order_relaxed)) return 0;
		if (!g_controlEnabled.load(std::memory_order_relaxed)) return 0;

		const int vk = (int)wParam;
		const int keyCode = vk;
		const int shiftKey = (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;
		const int ctrlKey = (GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
		const int altKey = (GetKeyState(VK_MENU) & 0x8000) ? 1 : 0;
		const int metaKey = ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) ? 1 : 0;

		json j = {
			{"type", "keyDown"},
			{"vk", vk},
			{"key", ""},
			{"code", ""},
			{"keyCode", keyCode},
			{"shiftKey", shiftKey},
			{"ctrlKey", ctrlKey},
			{"altKey", altKey},
			{"metaKey", metaKey},
		};
		SendToDataChannelAsync(j.dump());
		return 0;
	}
	case WM_KEYUP: {
		if (!g_inputArmed.load(std::memory_order_relaxed)) return 0;
		if (!g_controlEnabled.load(std::memory_order_relaxed)) return 0;

		const int vk = (int)wParam;
		const int keyCode = vk;
		const int shiftKey = (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;
		const int ctrlKey = (GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
		const int altKey = (GetKeyState(VK_MENU) & 0x8000) ? 1 : 0;
		const int metaKey = ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) ? 1 : 0;

		json j = {
			{"type", "keyUp"},
			{"vk", vk},
			{"key", ""},
			{"code", ""},
			{"keyCode", keyCode},
			{"shiftKey", shiftKey},
			{"ctrlKey", ctrlKey},
			{"altKey", altKey},
			{"metaKey", metaKey},
		};
		SendToDataChannelAsync(j.dump());
		return 0;
	}

	case WM_MOUSEMOVE: {
		if (!g_inputArmed.load(std::memory_order_relaxed)) return 0;
		if (!g_controlEnabled.load(std::memory_order_relaxed)) return 0;
		const int x = GET_X_LPARAM(lParam);
		const int y = GET_Y_LPARAM(lParam);
		if (!IsInsideVideoRect(x, y)) return 0;

		int absX = 0, absY = 0;
		MapClientToVideo(x, y, absX, absY);

		const int prevX = g_lastAbsX.load(std::memory_order_relaxed);
		const int prevY = g_lastAbsY.load(std::memory_order_relaxed);
		const int dx = (prevX < 0) ? 0 : (absX - prevX);
		const int dy = (prevY < 0) ? 0 : (absY - prevY);
		g_lastAbsX.store(absX, std::memory_order_relaxed);
		g_lastAbsY.store(absY, std::memory_order_relaxed);

		const int vw = g_videoW.load(std::memory_order_relaxed);
		const int vh = g_videoH.load(std::memory_order_relaxed);
		if (vw <= 0 || vh <= 0) return 0;

		json j = {
			{"type", "mouseMove"},
			{"x", dx},
			{"y", dy},
			{"absoluteX", absX},
			{"absoluteY", absY},
			{"videoWidth", vw},
			{"videoHeight", vh},
		};
		SendToDataChannelAsync(j.dump());
		return 0;
	}
	case WM_LBUTTONDOWN: {
		if (!g_controlEnabled.load(std::memory_order_relaxed)) return 0;
		const int x = GET_X_LPARAM(lParam);
		const int y = GET_Y_LPARAM(lParam);
		if (!IsInsideVideoRect(x, y)) return 0;
		g_inputArmed.store(true, std::memory_order_relaxed);
		SetFocus(hwnd);

		int absX = 0, absY = 0;
		MapClientToVideo(x, y, absX, absY);
		const int vw = g_videoW.load(std::memory_order_relaxed);
		const int vh = g_videoH.load(std::memory_order_relaxed);
		if (vw <= 0 || vh <= 0) return 0;

		json mv = {
			{"type", "mouseMove"},
			{"x", 0},
			{"y", 0},
			{"absoluteX", absX},
			{"absoluteY", absY},
			{"videoWidth", vw},
			{"videoHeight", vh},
		};
		SendToDataChannelAsync(mv.dump());

		json j = { {"type", "mouseDown"}, {"button", 0}, {"x", 0}, {"y", 0} };
		SendToDataChannelAsync(j.dump());
		return 0;
	}
	case WM_LBUTTONUP: {
		if (!g_inputArmed.load(std::memory_order_relaxed)) return 0;
		if (!g_controlEnabled.load(std::memory_order_relaxed)) return 0;
		const int x = GET_X_LPARAM(lParam);
		const int y = GET_Y_LPARAM(lParam);
		const bool inside = IsInsideVideoRect(x, y);
		if (inside) {
			int absX = 0, absY = 0;
			MapClientToVideo(x, y, absX, absY);
			const int vw = g_videoW.load(std::memory_order_relaxed);
			const int vh = g_videoH.load(std::memory_order_relaxed);
			if (vw > 0 && vh > 0) {
				json mv = {
					{"type", "mouseMove"},
					{"x", 0},
					{"y", 0},
					{"absoluteX", absX},
					{"absoluteY", absY},
					{"videoWidth", vw},
					{"videoHeight", vh},
				};
				SendToDataChannelAsync(mv.dump());
			}
		}

		json j = { {"type", "mouseUp"}, {"button", 0}, {"x", 0}, {"y", 0} };
		SendToDataChannelAsync(j.dump());
		return 0;
	}
	case WM_MOUSEWHEEL: {
		if (!g_inputArmed.load(std::memory_order_relaxed)) return 0;
		if (!g_controlEnabled.load(std::memory_order_relaxed)) return 0;
		POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hwnd, &pt);
		const int x = pt.x;
		const int y = pt.y;
		if (!IsInsideVideoRect(x, y)) return 0;

		const int deltaY = GET_WHEEL_DELTA_WPARAM(wParam);
		const int deltaX = 0;

		json j = { {"type", "mouseWheel"}, {"deltaX", deltaX}, {"deltaY", deltaY}, {"x", 0}, {"y", 0} };
		SendToDataChannelAsync(j.dump());
		return 0;
	}
	case WM_KILLFOCUS:
		g_inputArmed.store(false, std::memory_order_relaxed);
		g_lastAbsX.store(-1, std::memory_order_relaxed);
		g_lastAbsY.store(-1, std::memory_order_relaxed);
		return 0;
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED)
			RdrResizeD3D(hwnd);
		return 0;
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}
