#include "receiver_session.hpp"

#include "rdr_win32_include.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rtc/rtc.hpp>

#include "app_info.hpp"
#include "receiver_context.hpp"
#include "rdr_d3d.hpp"
#include "rdr_input.hpp"
#include "rdr_log.hpp"
#include "rdr_signaling.hpp"

static std::string RandomId(size_t length) {
	const char* chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	static thread_local std::mt19937_64 rng(std::random_device{}());
	std::uniform_int_distribution<size_t> dist(0, std::strlen(chars) - 1);
	std::string out;
	out.resize(length);
	for (size_t i = 0; i < length; ++i) out[i] = chars[dist(rng)];
	return out;
}

int ReceiverMain(int argc, char** argv) {
	std::string host = "192.168.3.15";
	int port = 9090;
	std::string clientId;
	std::string exePath = "C:\\Windows\\System32\\notepad.exe";
	RdrVideoDecoderMode decoderMode = RdrVideoDecoderMode::Auto;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--host" && i + 1 < argc) host = argv[++i];
		else if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
		else if (arg == "--clientId" && i + 1 < argc) clientId = argv[++i];
		else if (arg == "--exePath" && i + 1 < argc) exePath = argv[++i];
		else if (arg == "--decoder" && i + 1 < argc) {
			const std::string v = argv[++i];
			if (v == "hw" || v == "d3d11va") decoderMode = RdrVideoDecoderMode::Hw;
			else if (v == "sw" || v == "software") decoderMode = RdrVideoDecoderMode::Sw;
			else decoderMode = RdrVideoDecoderMode::Auto;
		}
		else if (arg == "--windowed") g_windowed = true;
		else if (arg == "--fullscreen") g_windowed = false;
		else if (arg == "--allMonitors") g_fullscreenAllMonitors = true;
		else if (arg == "--maxFps" && i + 1 < argc) g_maxPresentFps = std::atoi(argv[++i]);
		else if (arg == "--help" || arg == "-h") {
			std::printf(
				"Usage:\n"
				"  %s --host <host> [--port 9090] --exePath <path> [--clientId <id>]\n"
				"Display / performance:\n"
				"  (default)           Windowed mode; use taskbar / other apps normally\n"
				"  --fullscreen        Borderless fullscreen (primary or --allMonitors)\n"
				"  --windowed          Same as default (explicit)\n"
				"  --allMonitors       With --fullscreen: cover all monitors\n"
				"  --maxFps <n>        Cap present rate (default 60; 0 = unlimited)\n"
				"  --decoder <mode>    Video decode: auto (default), hw (D3D11VA), sw (CPU)\n"
				"Examples:\n"
				"  %s --host 127.0.0.1 --exePath \"C:\\\\Windows\\\\System32\\\\notepad.exe\"\n"
				"\n",
				rdr::kExecutableName,
				rdr::kExecutableName);
			return 0;
		}
	}

	if (clientId.empty()) clientId = RandomId(10);
	InitLogFile(clientId);
	SetUnhandledExceptionFilter(UnhandledExceptionLogger);
	std::printf("[receiver] clientId=%s\n", clientId.c_str());

	HINSTANCE hInstance = GetModuleHandle(nullptr);
	const wchar_t* className = rdr::kWindowClassName;
	WNDCLASSW wc{};
	wc.lpfnWndProc = RdrInputWndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = className;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc);

	if (g_windowed) {
		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const int workW = work.right - work.left;
		const int workH = work.bottom - work.top;
		int wantCw = std::min(1280, (workW * 4) / 5);
		int wantCh = std::min(720, (workH * 4) / 5);
		wantCw = std::max(640, std::min(wantCw, workW));
		wantCh = std::max(360, std::min(wantCh, workH));

		RECT adj{ 0, 0, wantCw, wantCh };
		AdjustWindowRectEx(&adj, WS_OVERLAPPEDWINDOW, FALSE, 0);
		const int winW = adj.right - adj.left;
		const int winH = adj.bottom - adj.top;
		const int x = work.left + (workW - winW) / 2;
		const int y = work.top + (workH - winH) / 2;

		g_hwnd = CreateWindowExW(
			0, className, rdr::kDefaultWindowTitle,
			WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			x, y, winW, winH,
			nullptr, nullptr, hInstance, nullptr);
	} else {
		int sx = 0, sy = 0, sw = 0, sh = 0;
		if (g_fullscreenAllMonitors) {
			sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
			sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
			sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
			sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		} else {
			MONITORINFOEXW mi{};
			mi.cbSize = sizeof(mi);
			HMONITOR hMon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
			if (hMon && GetMonitorInfoW(hMon, &mi)) {
				const RECT& r = mi.rcMonitor;
				sx = r.left;
				sy = r.top;
				sw = r.right - r.left;
				sh = r.bottom - r.top;
			} else {
				sw = GetSystemMetrics(SM_CXSCREEN);
				sh = GetSystemMetrics(SM_CYSCREEN);
			}
		}
		g_hwnd = CreateWindowExW(
			0, className, rdr::kDefaultWindowTitle,
			WS_POPUP | WS_VISIBLE, sx, sy, sw, sh,
			nullptr, nullptr, hInstance, nullptr);
		if (g_fullscreenAllMonitors)
			RdrApplyFullscreenAllMonitors(g_hwnd);
		else
			RdrApplyFullscreenPrimary(g_hwnd);
	}
	if (!g_hwnd) {
		Logf("CreateWindowExW failed\n");
		return 1;
	}
	ShowWindow(g_hwnd, SW_SHOW);
	UpdateWindow(g_hwnd);

	LARGE_INTEGER freq{};
	LARGE_INTEGER qpc0{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&qpc0);
	g_qpcFreq = (uint64_t)freq.QuadPart;
	g_baseQpc = (uint64_t)qpc0.QuadPart;
	g_baseSysMs = SystemMsNow();

	try {
		rtc::InitLogger(rtc::LogLevel::Info);
		RdrEnsureD3D(g_hwnd);
		RdrUpdateLetterboxRect();
	} catch (const std::exception& e) {
		Logf("Init D3D failed: %s\n", e.what());
		return 2;
	}

	std::thread t([&] {
		try {
			RdrRunSignalingAndWebRtc(host, port, clientId, exePath, decoderMode);
		} catch (const std::exception& e) {
			Logf("WebRTC thread error: %s\n", e.what());
			g_exitRequested.store(true, std::memory_order_relaxed);
			PostMessage(g_hwnd, WM_CLOSE, 0, 0);
		}
	});
	t.detach();

	uint64_t lastShownDecodedIndex = 0;
	uint64_t frameCapMs = 0;
	uint64_t frameEncMs = 0;
	uint64_t frameSendMs = 0;
	bool haveFrameAgentTimes = false;
	uint64_t frameId = 0;

	uint64_t frameRxMs = 0;
	uint64_t frameDecDoneMs = 0;
	bool haveFrameRxDecTimes = false;

	std::vector<uint8_t> localBgra;

	MSG msg{};
	static uint64_t g_debugPrintedFrames = 0;
	while (!g_exitRequested.load(std::memory_order_relaxed)) {
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		if (g_exitRequested.load(std::memory_order_relaxed)) break;

		DrainDcSendPending();

		bool haveFrame = false;
		int w = 0;
		int h = 0;
		uint64_t decodedIndex = 0;
		bool haveDecodeQueuedSteady = false;
		std::chrono::steady_clock::time_point decodeQueuedSteady{};
		constexpr int kMaxFrameDrain = 8;
		for (int drain = 0; drain < kMaxFrameDrain; ++drain) {
			bool got = false;
			{
				std::lock_guard<std::mutex> lk(g_frameMtx);
				if (g_sharedFrame.ready && g_sharedFrame.decodedIndex != lastShownDecodedIndex) {
					got = true;
					w = g_sharedFrame.w;
					h = g_sharedFrame.h;
					decodedIndex = g_sharedFrame.decodedIndex;
					frameId = g_sharedFrame.frameId;
					haveFrameAgentTimes = g_sharedFrame.hasAgentTimes;
					frameCapMs = g_sharedFrame.capMs;
					frameEncMs = g_sharedFrame.encMs;
					frameSendMs = g_sharedFrame.sendMs;

					haveFrameRxDecTimes = g_sharedFrame.hasRxDecTimes;
					frameRxMs = g_sharedFrame.rxMs;
					frameDecDoneMs = g_sharedFrame.decDoneMs;
					if (g_sharedFrame.hasDecodeQueuedSteady) {
						decodeQueuedSteady = g_sharedFrame.decodeQueuedSteady;
						haveDecodeQueuedSteady = true;
						g_sharedFrame.hasDecodeQueuedSteady = false;
					}
					localBgra.swap(g_sharedFrame.bgra);
					g_sharedFrame.hasAgentTimes = false;
					g_sharedFrame.hasRxDecTimes = false;
					g_sharedFrame.ready = false;
					lastShownDecodedIndex = decodedIndex;
				}
			}
			if (!got)
				break;
			haveFrame = true;
		}

		if (!haveFrame) {
			(void)MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, 0);
			continue;
		}

		if (g_debugPrintedFrames < 5) {
			Logf("[receiver] show frame idx=%llu size=%dx%d\n",
				(unsigned long long)decodedIndex, w, h);
			++g_debugPrintedFrames;
		}
		RdrResizeWindowToVideoResolution(g_hwnd, w, h);

		const auto t_render_begin = std::chrono::steady_clock::now();
		const bool logStages = (decodedIndex <= 2);
		try {
			if (logStages) Logf("[receiver] stage EnsureTextureIfNeeded begin\n");
			RdrEnsureTextureIfNeeded(w, h);
			if (logStages) Logf("[receiver] stage EnsureTextureIfNeeded end\n");
			if (g_d3d.texture) {
				if (logStages) Logf("[receiver] stage UpdateSubresource begin\n");
				g_d3d.context->UpdateSubresource(
					g_d3d.texture.Get(),
					0, nullptr,
					localBgra.data(),
					w * 4,
					0);
				if (logStages) Logf("[receiver] stage UpdateSubresource end\n");
			}
			RdrUpdateLetterboxRectFor(w, h);

			if (logStages) Logf("[receiver] stage RenderOneFrame begin\n");
			RdrRenderOneFrame(w, h);
			if (logStages) Logf("[receiver] stage RenderOneFrame end\n");

			if (logStages) Logf("[receiver] stage Present begin\n");
			(void)g_d3d.swapChain->Present(0, 0);
			if (logStages) Logf("[receiver] stage Present end\n");
		} catch (const std::exception& e) {
			Logf("[receiver] render exception: %s\n", e.what());
			g_exitRequested.store(true, std::memory_order_relaxed);
			if (g_hwnd) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
			break;
		} catch (...) {
			Logf("[receiver] render exception: unknown\n");
			g_exitRequested.store(true, std::memory_order_relaxed);
			if (g_hwnd) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
			break;
		}
		LARGE_INTEGER qpcNow{};
		QueryPerformanceCounter(&qpcNow);
		const uint64_t presentQpc = (uint64_t)qpcNow.QuadPart;
		const uint64_t presentMs = QpcToSysMs(presentQpc);

		const auto t_render_end = std::chrono::steady_clock::now();
		{
			const int render_us = static_cast<int>(
				std::chrono::duration_cast<std::chrono::microseconds>(t_render_end - t_render_begin).count());
			int queue_wait_us = -1;
			int decode_to_present_us = -1;
			if (haveDecodeQueuedSteady) {
				queue_wait_us = static_cast<int>(
					std::chrono::duration_cast<std::chrono::microseconds>(t_render_begin - decodeQueuedSteady).count());
				decode_to_present_us = static_cast<int>(
					std::chrono::duration_cast<std::chrono::microseconds>(t_render_end - decodeQueuedSteady).count());
			}
			static std::chrono::steady_clock::time_point s_last_present_log{};
			if (t_render_end - s_last_present_log >= std::chrono::seconds(1)) {
				s_last_present_log = t_render_end;
				Logf("[latency][recv_present] idx=%llu queue_wait_us=%d render_us=%d decode_to_present_us=%d sysMs=%llu\n",
					(unsigned long long)decodedIndex,
					queue_wait_us,
					render_us,
					decode_to_present_us,
					(unsigned long long)SystemMsNow());
			}
		}

		if (g_maxPresentFps > 0) {
			using namespace std::chrono;
			static steady_clock::time_point nextCap = steady_clock::now();
			const int fps = g_maxPresentFps;
			const int denom = (fps > 0) ? fps : 1;
			const auto period = microseconds(1000000 / denom);
			const auto now = steady_clock::now();
			if (now < nextCap)
				std::this_thread::sleep_until(nextCap);
			nextCap += period;
			const auto after = steady_clock::now();
			if (nextCap < after)
				nextCap = after;
		}

		double thetaLocal = 0.0;
		bool hasTheta = false;
		{
			std::lock_guard<std::mutex> lk(g_latency.mtx);
			hasTheta = g_latency.thetaMs.has_value();
			thetaLocal = hasTheta ? g_latency.thetaMs.value() : 0.0;
		}

		double capToPresentMs = 0.0;
		double encToPresentMs = 0.0;
		double sendToPresentMs = 0.0;
		if (haveFrameAgentTimes && hasTheta) {
			capToPresentMs = (double)presentMs - (double)frameCapMs + thetaLocal;
			encToPresentMs = (double)presentMs - (double)frameEncMs + thetaLocal;
			sendToPresentMs = (double)presentMs - (double)frameSendMs + thetaLocal;
		}

		double rxToDecMs = 0.0;
		double decToPresentMs = 0.0;
		if (haveFrameRxDecTimes) {
			rxToDecMs = (double)frameDecDoneMs - (double)frameRxMs;
			decToPresentMs = (double)presentMs - (double)frameDecDoneMs;
		}

		{
			static std::chrono::steady_clock::time_point s_last_visible_log{};
			const auto nowSteady = std::chrono::steady_clock::now();
			//if (nowSteady - s_last_visible_log >= std::chrono::seconds(1)) 
			{
				s_last_visible_log = nowSteady;
				Logf("[latency][seg] idx=%llu frameId=%llu cap->present=%.1fms enc->present=%.1fms send->present=%.1fms rx->dec=%.1fms dec->present=%.1fms theta=%.1fms\n",
					(unsigned long long)decodedIndex,
					(unsigned long long)frameId,
					capToPresentMs,
					encToPresentMs,
					sendToPresentMs,
					rxToDecMs,
					decToPresentMs,
					thetaLocal);
			}
		}

		wchar_t title[256]{};
		if (haveFrameAgentTimes && hasTheta) {
			std::swprintf(
				title, 256, L"%ls | cap->present %.1fms (enc %.1fms send %.1fms) theta=%.1fms frameId=%I64u",
				rdr::kDefaultWindowTitle,
				capToPresentMs,
				encToPresentMs,
				sendToPresentMs,
				thetaLocal,
				(unsigned __int64)frameId);
		} else if (hasTheta) {
			std::swprintf(title, 256, L"%ls | waiting SEI times...", rdr::kDefaultWindowTitle);
		} else {
			std::swprintf(title, 256, L"%ls | waiting latPong/align...", rdr::kDefaultWindowTitle);
		}
		SetWindowTextW(g_hwnd, title);
	}

	g_exitRequested.store(true, std::memory_order_relaxed);
	return 0;
}
