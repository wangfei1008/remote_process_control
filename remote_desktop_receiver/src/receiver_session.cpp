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

namespace {
	static void InitReceiverRunState(const ReceiverOptions& opt, std::string& inoutClientId) {
		// Global display flags used by the Win32/D3D layer.
		g_windowed = opt.windowed;
		g_fullscreenAllMonitors = opt.fullscreenAllMonitors;
		g_maxPresentFps = opt.maxPresentFps;

		if (inoutClientId.empty()) inoutClientId = RandomId(10);
		InitLogFile(inoutClientId);
	}

	static bool CreateReceiverWindow() {
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

			RECT adj{0, 0, wantCw, wantCh};
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

		if (!g_hwnd) return false;
		ShowWindow(g_hwnd, SW_SHOW);
		UpdateWindow(g_hwnd);
		return true;
	}

	static void InitQpcSysMsBase() {
		LARGE_INTEGER freq{};
		LARGE_INTEGER qpc0{};
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&qpc0);
		g_qpcFreq = (uint64_t)freq.QuadPart;
		g_baseQpc = (uint64_t)qpc0.QuadPart;
		g_baseSysMs = SystemMsNow();
	}

	static bool InitD3DOnceForWindow(HWND hwnd) {
		try {
			rtc::InitLogger(rtc::LogLevel::Info);
			RdrEnsureD3D(hwnd);
			RdrUpdateLetterboxRect();
			return true;
		} catch (const std::exception& e) {
			Logf("Init D3D failed: %s\n", e.what());
			return false;
		}
	}

	static void StartWebRtcThreadDetached(const ReceiverOptions& opt, const std::string& clientId) {
		const std::string host = opt.host;
		const int port = opt.port;
		const std::string clientIdCopy = clientId;
		const std::string exePath = opt.exePath;
		const RdrVideoDecoderMode decoderMode = opt.decoderMode;

		std::thread t([host, port, clientIdCopy, exePath, decoderMode] {
			try {
				RdrRunSignalingAndWebRtc(host, port, clientIdCopy, exePath, decoderMode);
			} catch (const std::exception& e) {
				Logf("WebRTC thread error: %s\n", e.what());
				g_exitRequested.store(true, std::memory_order_relaxed);
				PostMessage(g_hwnd, WM_CLOSE, 0, 0);
			}
		});
		t.detach();
	}
} // namespace

int ReceiverMain(const ReceiverOptions& opt) {
	// 执行步骤概览：
	// 1) 初始化运行态（全局显示标志 + clientId + 日志文件）与异常捕获
	// 2) 创建窗口（窗口/全屏/多显示器）
	// 3) 初始化本机 QPC->sysMs 换算基准
	// 4) 初始化 D3D 渲染资源
	// 5) 启动 WebRTC/信令线程（负责接收/解码并写入共享帧缓冲）
	// 6) 主线程循环：取最新帧 -> 上传纹理 -> 渲染 -> Present -> 打印本机渲染耗时
	std::string clientId = opt.clientId;
	InitReceiverRunState(opt, clientId);
	SetUnhandledExceptionFilter(UnhandledExceptionLogger);
	std::printf("[receiver] clientId=%s\n", clientId.c_str());

	// Step 2: 创建窗口（g_hwnd）。
	if (!CreateReceiverWindow()) {
		Logf("CreateWindowExW failed\n");
		return 1;
	}

	// Step 3: 初始化本机 QPC 基准（用于把 QPC 换算成 sysMs）。
	InitQpcSysMsBase();

	// Step 4: 初始化 D3D（swapchain/纹理/渲染管线等）。
	if (!InitD3DOnceForWindow(g_hwnd)) {
		return 2;
	}

	// Step 5: 启动 WebRTC/信令线程；视频帧解码完成后会写入 g_sharedFrame。
	StartWebRtcThreadDetached(opt, clientId);

	uint64_t lastShownDecodedIndex = 0;
	uint64_t frameId = 0;

	std::vector<uint8_t> localBgra;

	MSG msg{};
	static uint64_t g_debugPrintedFrames = 0;
	while (!g_exitRequested.load(std::memory_order_relaxed)) {
		// Step 6.1: Win32 消息泵（窗口/输入），避免 UI 无响应。
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		if (g_exitRequested.load(std::memory_order_relaxed)) break;

		// Step 6.2: 处理数据通道待发送（输入/控制等）。
		DrainDcSendPending();

		// Step 6.3: 尽量取到“最新未展示帧”（多次 drain 以追上 producer）。
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
					if (g_sharedFrame.hasDecodeQueuedSteady) {
						decodeQueuedSteady = g_sharedFrame.decodeQueuedSteady;
						haveDecodeQueuedSteady = true;
						g_sharedFrame.hasDecodeQueuedSteady = false;
					}
					localBgra.swap(g_sharedFrame.bgra);
					g_sharedFrame.ready = false;
					lastShownDecodedIndex = decodedIndex;
				}
			}
			if (!got)
				break;
			haveFrame = true;
		}

		if (!haveFrame) {
			// 无新帧：短暂等待或被窗口消息唤醒，降低空转。
			(void)MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, 0);
			continue;
		}

		if (g_debugPrintedFrames < 5) {
			Logf("[receiver] show frame idx=%llu size=%dx%d\n", (unsigned long long)decodedIndex, w, h);
			++g_debugPrintedFrames;
		}
		// Step 6.4: 如有必要，调整窗口大小以匹配视频分辨率。
		RdrResizeWindowToVideoResolution(g_hwnd, w, h);

		// Step 6.5: 上传 BGRA 到纹理 -> 绘制 -> Present。
		const bool logStages = (decodedIndex <= 2);
		try {
			if (logStages) Logf("[receiver] stage EnsureTextureIfNeeded begin\n");
			RdrEnsureTextureIfNeeded(w, h);
			if (logStages) Logf("[receiver] stage EnsureTextureIfNeeded end\n");
			if (g_d3d.texture) {
				if (logStages) Logf("[receiver] stage UpdateSubresource begin\n");
				g_d3d.context->UpdateSubresource(g_d3d.texture.Get(), 0, nullptr, localBgra.data(),	w * 4,	0);
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
	}

	g_exitRequested.store(true, std::memory_order_relaxed);
	return 0;
}
