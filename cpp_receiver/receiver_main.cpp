#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// Some toolchains may not expose GET_X_LPARAM/GET_Y_LPARAM as macros.
// Define fallbacks to keep this receiver self-contained.
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <optional>
#include <cstdarg>
#include <stdexcept>
#include <random>
#include <string>
#include <queue>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include <rtc/rtc.hpp>
#include <rtc/h264rtpdepacketizer.hpp>

#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ws2_32.lib")

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;
static std::atomic<bool> g_logReady{false};

static void Logf(const char* fmt, ...) {
	if (!g_logReady.load(std::memory_order_relaxed)) return;
	char buf[2048];
	va_list args;
	va_start(args, fmt);
#if defined(_MSC_VER)
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
#else
	std::vsnprintf(buf, sizeof(buf), fmt, args);
#endif
	va_end(args);
	if (buf[0] == '\0') return;
	EnterCriticalSection(&g_logCs);
	DWORD written = 0;
	(void)WriteFile(g_logFile, buf, (DWORD)std::strlen(buf), &written, nullptr);
	LeaveCriticalSection(&g_logCs);
}

static LONG WINAPI UnhandledExceptionLogger(EXCEPTION_POINTERS* ep) {
	// Best-effort log for access violations / crashes.
	// This runs when C++ exceptions aren't available.
	DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
	void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
	Logf("[receiver] unhandled exception code=0x%08lX addr=%p\n", (unsigned long)code, addr);
	return EXCEPTION_EXECUTE_HANDLER; // let the process terminate after logging
}

static void EnsureWinsockOnce() {
	static std::once_flag once;
	std::call_once(once, [] {
		WSADATA wsa{};
		const int r = WSAStartup(MAKEWORD(2, 2), &wsa);
		if (r != 0) Logf("[net] WSAStartup failed: %d\n", r);
	});
}

static std::atomic<bool> g_exitRequested{false};
static HWND g_hwnd = nullptr;
static std::atomic<bool> g_pingThreadStarted{false};

/** Normal window with title bar: default on so F5/debug does not block the whole desktop. */
static bool g_windowed = true;
/** Fullscreen cover every monitor (old behavior). Default false: only primary monitor. */
static bool g_fullscreenAllMonitors = false;
/** Cap render/present rate to reduce CPU/GPU load (0 = unlimited, for latency experiments). */
static int g_maxPresentFps = 60;

struct SharedVideoFrame {
	std::vector<uint8_t> bgra; // size = w*h*4
	int w = 0;
	int h = 0;
	uint64_t decodedIndex = 0;
	bool ready = false;
};

static std::mutex g_frameMtx;
static SharedVideoFrame g_sharedFrame;

static std::recursive_mutex g_dcMtx;
static std::shared_ptr<rtc::DataChannel> g_dataChannel;
// libdatachannel 里 Track 对象的生命周期不保证跨回调持续。
// 为了确保 onFrame 能持续收到数据，需要在 receiver 端保留一份 shared_ptr 引用。


static std::shared_ptr<rtc::Track> g_videoTrackKeepAlive;

// Outgoing websocket messages are also queued to avoid calling ws->send
// directly from inside libdatachannel callbacks.
static std::mutex g_wsMtx;
static std::shared_ptr<rtc::WebSocket> g_wsForRequest;
static std::mutex g_wsSendPendingMtx;
static std::queue<std::string> g_wsSendPending;

static std::atomic<bool> g_controlEnabled{false};
static std::atomic<bool> g_inputArmed{false};
static std::atomic<bool> g_videoTrackAttached{false};
static std::atomic<int> g_lastAbsX{-1};
static std::atomic<int> g_lastAbsY{-1};

static std::atomic<int> g_videoW{0};
static std::atomic<int> g_videoH{0};
static std::atomic<int> g_lbX{0};
static std::atomic<int> g_lbY{0};
static std::atomic<int> g_lbW{0};
static std::atomic<int> g_lbH{0};

struct LatencyState {
	std::mutex mtx;

	std::optional<double> thetaMs; // smoothed
	uint64_t lastFrameMarkSeq = 0;
	uint64_t lastFrameMarkSrvMs = 0;
	bool hasLastFrameMark = false;

	std::unordered_map<uint64_t, uint64_t> srvMsBySeq; // seq -> srvMs(epoch ms)

	void prune(uint64_t currentSeq) {
		// Keep a sliding window to avoid unbounded growth.
		constexpr uint64_t keepBehind = 2500;
		uint64_t minSeq = (currentSeq > keepBehind) ? (currentSeq - keepBehind) : 0;
		for (auto it = srvMsBySeq.begin(); it != srvMsBySeq.end();) {
			if (it->first < minSeq) it = srvMsBySeq.erase(it);
			else ++it;
		}
	}
};

static LatencyState g_latency;

static double SmoothEwma(std::optional<double>& prev, double sample, double alpha) {
	if (!prev.has_value()) prev = sample;
	else prev = prev.value() * (1.0 - alpha) + sample * alpha;
	return prev.value();
}

static std::string RandomId(size_t length) {
	const char* chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	static thread_local std::mt19937_64 rng(std::random_device{}());
	std::uniform_int_distribution<size_t> dist(0, std::strlen(chars) - 1);
	std::string out;
	out.resize(length);
	for (size_t i = 0; i < length; ++i) out[i] = chars[dist(rng)];
	return out;
}

static uint64_t SystemMsNow() {
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static void InitLogFile(const std::string& clientId) {
	// Log file write via WinAPI to avoid depending on CRT stdout/stderr state.
	const std::string logPath =
		"cpp_receiver_" + clientId + "_" + std::to_string((unsigned long long)GetCurrentProcessId()) + ".log";

	// clientId is ascii-ish (digits/letters), so CreateFileA is safe here.
	g_logFile = CreateFileA(
		logPath.c_str(),
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (g_logFile == INVALID_HANDLE_VALUE) return;

	InitializeCriticalSection(&g_logCs);
	g_logReady.store(true, std::memory_order_release);

	Logf("[log] started clientId=%s pid=%llu\n", clientId.c_str(), (unsigned long long)GetCurrentProcessId());
}

class H264AnnexBDecoderToBGRA {
public:
	H264AnnexBDecoderToBGRA() {
		const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) throw std::runtime_error("FFmpeg: H264 decoder not found");
		m_ctx = avcodec_alloc_context3(codec);
		if (!m_ctx) throw std::runtime_error("FFmpeg: alloc context failed");
		m_ctx->thread_count = 1;
		if (avcodec_open2(m_ctx, codec, nullptr) < 0) throw std::runtime_error("FFmpeg: open2 failed");
		m_packet = av_packet_alloc();
		m_frame = av_frame_alloc();
		if (!m_packet || !m_frame) throw std::runtime_error("FFmpeg: alloc packet/frame failed");
	}

	~H264AnnexBDecoderToBGRA() {
		if (m_sws) sws_freeContext(m_sws);
		if (m_ctx) avcodec_free_context(&m_ctx);
		if (m_packet) av_packet_free(&m_packet);
		if (m_frame) av_frame_free(&m_frame);
	}

	// Decode one AnnexB access unit. Returns true when a frame is available.
	bool decodeAnnexB(const uint8_t* data, size_t size) {
		std::lock_guard<std::mutex> lk(m_decodeMtx);

		if (!m_ctx) return false;

		av_packet_unref(m_packet);
		if (av_new_packet(m_packet, (int)size) < 0) return false;
		std::memcpy(m_packet->data, data, size);

		int ret = avcodec_send_packet(m_ctx, m_packet);
		if (ret < 0) {
		Logf("[decoder] avcodec_send_packet failed ret=%d\n", ret);
			return false;
		}

		ret = avcodec_receive_frame(m_ctx, m_frame);
		if (ret != 0) {
			return false; // EAGAIN/EOF or decode not ready yet
		}

		const int outW = m_frame->width;
		const int outH = m_frame->height;
		if (outW <= 0 || outH <= 0) return false;

		ensureSws(outW, outH, static_cast<AVPixelFormat>(m_frame->format));

		uint8_t* dstSlices[4] = { m_bgraTmp.data(), nullptr, nullptr, nullptr };
		int dstStride[4] = { outW * 4, 0, 0, 0 };
		sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, outH, dstSlices, dstStride);

		// Publish to renderer: swap buffers to avoid copying.
		{
			std::lock_guard<std::mutex> lk2(g_frameMtx);
			g_sharedFrame.w = outW;
			g_sharedFrame.h = outH;
			g_sharedFrame.decodedIndex++;
			g_sharedFrame.bgra.swap(m_bgraTmp);
			g_sharedFrame.ready = true;
		}
		g_videoW.store(outW, std::memory_order_relaxed);
		g_videoH.store(outH, std::memory_order_relaxed);
		return true;
	}

private:
	void ensureSws(int outW, int outH, AVPixelFormat srcFmt) {
		// Even when sws context is unchanged, m_bgraTmp might have been swapped out
		// into the shared frame buffer, leaving it empty or wrong-sized.
		// Ensure the backing storage size always matches outW*outH*4 before sws_scale.
		if (m_sws && outW == m_outW && outH == m_outH && srcFmt == m_srcFmt) {
			const size_t need = (size_t)outW * (size_t)outH * 4;
			if (m_bgraTmp.size() != need) m_bgraTmp.resize(need);
			return;
		}

		if (m_sws) sws_freeContext(m_sws);
		m_sws = sws_getContext(outW, outH, srcFmt,
			outW, outH, AV_PIX_FMT_BGRA,
			SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!m_sws) throw std::runtime_error("FFmpeg: sws_getContext failed");

		m_outW = outW;
		m_outH = outH;
		m_srcFmt = srcFmt;

		m_bgraTmp.resize((size_t)outW * (size_t)outH * 4);
	}

	std::mutex m_decodeMtx;
	AVCodecContext* m_ctx = nullptr;
	AVPacket* m_packet = nullptr;
	AVFrame* m_frame = nullptr;
	SwsContext* m_sws = nullptr;
	int m_outW = 0;
	int m_outH = 0;
	AVPixelFormat m_srcFmt = AV_PIX_FMT_NONE;
	std::vector<uint8_t> m_bgraTmp;
};

struct D3DResources {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGISwapChain> swapChain;
	ComPtr<ID3D11RenderTargetView> rtv;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps;
	ComPtr<ID3D11InputLayout> inputLayout;
	ComPtr<ID3D11Buffer> vertexBuffer;
	ComPtr<ID3D11Buffer> scaleCB;

	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> textureSRV;
	ComPtr<ID3D11SamplerState> sampler;
};

static D3DResources g_d3d;
static int g_clientW = 0;
static int g_clientH = 0;

struct Vertex {
	float px;
	float py;
	float u;
	float v;
};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static uint64_t g_baseQpc = 0;
static uint64_t g_baseSysMs = 0;
static uint64_t g_qpcFreq = 0;

static uint64_t QpcToSysMs(uint64_t qpc) {
	if (g_qpcFreq == 0) return 0;
	// presentTimeMs = baseSysMs + (qpc - baseQpc)*1000/freq
	return g_baseSysMs + (uint64_t)(((double)(qpc - g_baseQpc) * 1000.0) / (double)g_qpcFreq);
}

static bool SendToDataChannel(const std::string& s) {
	std::shared_ptr<rtc::DataChannel> dc;
	{
		std::lock_guard<std::recursive_mutex> lk(g_dcMtx);
		dc = g_dataChannel;
	}
	if (!dc || !dc->isOpen()) return false;
	try {
		return dc->send(s);
	} catch (...) {
		return false;
	}
}

// Push outgoing messages from callbacks into a queue,
// then drain them on the main/render thread to avoid re-entrancy.
// Use recursive_mutex to avoid "resource deadlock would occur" when callbacks
// are re-entered on the same thread by libdatachannel internals.
static std::recursive_mutex g_dcSendPendingMtx;
static std::queue<std::string> g_dcSendPending;

static void SendToDataChannelAsync(std::string s) {
	std::lock_guard<std::recursive_mutex> lk(g_dcSendPendingMtx);
	g_dcSendPending.push(std::move(s));
}

static void DrainDcSendPending() {
	std::queue<std::string> local;
	{
		std::lock_guard<std::recursive_mutex> lk(g_dcSendPendingMtx);
		std::swap(local, g_dcSendPending);
	}
	while (!local.empty()) {
		SendToDataChannel(local.front());
		local.pop();
	}
}

static void DrainWsSendPending() {
	std::queue<std::string> local;
	{
		std::lock_guard<std::mutex> lk(g_wsSendPendingMtx);
		std::swap(local, g_wsSendPending);
	}
	if (local.empty()) return;

	std::shared_ptr<rtc::WebSocket> ws;
	{
		std::lock_guard<std::mutex> lk(g_wsMtx);
		ws = g_wsForRequest;
	}
	if (!ws || !ws->isOpen()) return;

	static std::atomic<int> wsSendLogCount{0};
	const size_t cnt = local.size();
	if (wsSendLogCount.load(std::memory_order_relaxed) < 5) {
		Logf("[signaling] DrainWsSendPending begin cnt=%llu\n", (unsigned long long)cnt);
	}

	while (!local.empty()) {
		try {
			if (wsSendLogCount.load(std::memory_order_relaxed) < 5) {
				// log only for first few messages to avoid huge logs
				Logf("[signaling] DrainWsSendPending sending one\n");
				wsSendLogCount.fetch_add(1, std::memory_order_relaxed);
			}
			const std::string& payload = local.front();
			static std::atomic<int> wsPayloadLogCnt{0};
			const int plc = wsPayloadLogCnt.fetch_add(1, std::memory_order_relaxed);
			if (plc < 5) {
				const size_t n = (payload.size() < 160) ? payload.size() : 160;
				Logf("[signaling] ws payload head(%llu)=%.*s\n",
					(unsigned long long)payload.size(), (int)n, payload.c_str());
			}
			const bool ok = ws->send(payload);
			static std::atomic<int> wsSendOkCnt{0};
			const int k = wsSendOkCnt.fetch_add(1, std::memory_order_relaxed);
			if (k < 5) {
				Logf("[signaling] DrainWsSendPending ws->send ok=%d bytes=%llu\n",
					(int)ok, (unsigned long long)payload.size());
			}
		} catch (...) {
			Logf("[signaling] DrainWsSendPending ws->send failed (caught exception)\n");
		}
		local.pop();
	}
}

static void UpdateLetterboxRect() {
	const int vw = g_videoW.load(std::memory_order_relaxed);
	const int vh = g_videoH.load(std::memory_order_relaxed);
	if (vw <= 0 || vh <= 0 || g_clientW <= 0 || g_clientH <= 0) return;

	const double winAspect = (double)g_clientW / (double)g_clientH;
	const double vidAspect = (double)vw / (double)vh;

	double contentW = 0.0;
	double contentH = 0.0;
	if (winAspect >= vidAspect) {
		contentH = (double)g_clientH;
		contentW = contentH * vidAspect;
	} else {
		contentW = (double)g_clientW;
		contentH = contentW / vidAspect;
	}

	const int x = (int)std::round(((double)g_clientW - contentW) / 2.0);
	const int y = (int)std::round(((double)g_clientH - contentH) / 2.0);
	const int w = (int)std::max(1.0, std::round(contentW));
	const int h = (int)std::max(1.0, std::round(contentH));

	g_lbX.store(x, std::memory_order_relaxed);
	g_lbY.store(y, std::memory_order_relaxed);
	g_lbW.store(w, std::memory_order_relaxed);
	g_lbH.store(h, std::memory_order_relaxed);
}

static void UpdateLetterboxRectFor(int vw, int vh) {
	if (vw <= 0 || vh <= 0 || g_clientW <= 0 || g_clientH <= 0) return;

	const double winAspect = (double)g_clientW / (double)g_clientH;
	const double vidAspect = (double)vw / (double)vh;

	double contentW = 0.0;
	double contentH = 0.0;
	if (winAspect >= vidAspect) {
		contentH = (double)g_clientH;
		contentW = contentH * vidAspect;
	} else {
		contentW = (double)g_clientW;
		contentH = contentW / vidAspect;
	}

	const int x = (int)std::round(((double)g_clientW - contentW) / 2.0);
	const int y = (int)std::round(((double)g_clientH - contentH) / 2.0);
	const int w = (int)std::max(1.0, std::round(contentW));
	const int h = (int)std::max(1.0, std::round(contentH));

	g_lbX.store(x, std::memory_order_relaxed);
	g_lbY.store(y, std::memory_order_relaxed);
	g_lbW.store(w, std::memory_order_relaxed);
	g_lbH.store(h, std::memory_order_relaxed);
}

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
	outAbsX = std::max(0, std::min(vw - 1, ax));
	outAbsY = std::max(0, std::min(vh - 1, ay));
}

static void EnsureD3D(HWND hwnd) {
	RECT rc{};
	GetClientRect(hwnd, &rc);
	g_clientW = rc.right - rc.left;
	g_clientH = rc.bottom - rc.top;

	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL outLevel{};

	DXGI_SWAP_CHAIN_DESC scd{};
	scd.BufferCount = 2;
	scd.BufferDesc.Width = g_clientW;
	scd.BufferDesc.Height = g_clientH;
	scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	scd.BufferDesc.RefreshRate.Numerator = 0;
	scd.BufferDesc.RefreshRate.Denominator = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.OutputWindow = hwnd;
	scd.SampleDesc.Count = 1;
	scd.SampleDesc.Quality = 0;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		flags, levels, 1, D3D11_SDK_VERSION,
		&scd, &g_d3d.swapChain, &g_d3d.device, &outLevel, &g_d3d.context);
	if (FAILED(hr)) {
		throw std::runtime_error("D3D11CreateDeviceAndSwapChain failed");
	}

	ComPtr<ID3D11Texture2D> backBuffer;
	hr = g_d3d.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
	if (FAILED(hr)) throw std::runtime_error("GetBuffer backBuffer failed");

	hr = g_d3d.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_d3d.rtv);
	if (FAILED(hr)) throw std::runtime_error("CreateRenderTargetView failed");

	// Fullscreen quad shaders
	const char* vsSrc =
		"cbuffer cb0: register(b0){ float2 scale; };"
		"struct VSIn{ float2 pos: POSITION; float2 uv: TEXCOORD0; };"
		"struct VSOut{ float4 pos: SV_POSITION; float2 uv: TEXCOORD0; };"
		"VSOut main(VSIn vin){ VSOut o; o.pos=float4(vin.pos*scale,0,1); o.uv=vin.uv; return o; }";

	const char* psSrc =
		"Texture2D tex0: register(t0);"
		"SamplerState sam0: register(s0);"
		"struct PSIn{ float4 pos: SV_POSITION; float2 uv: TEXCOORD0; };"
		"float4 main(PSIn pin): SV_TARGET{ return tex0.Sample(sam0, pin.uv); }";

	ComPtr<ID3DBlob> vsBlob;
	ComPtr<ID3DBlob> psBlob;
	ComPtr<ID3DBlob> errBlob;

	auto compileShader = [&](const char* src, const char* entry, const char* profile, ComPtr<ID3DBlob>& outBlob) {
		UINT compileFlags = 0;
#ifdef _DEBUG
		compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
		HRESULT hr2 = D3DCompile(
			src, std::strlen(src), nullptr, nullptr, nullptr,
			entry, profile, compileFlags, 0, &outBlob, &errBlob);
		if (FAILED(hr2)) {
			if (errBlob) Logf("[d3d] shader compile error: %s\n", (const char*)errBlob->GetBufferPointer());
			throw std::runtime_error("D3DCompile failed");
		}
	};

	compileShader(vsSrc, "main", "vs_4_0", vsBlob);
	compileShader(psSrc, "main", "ps_4_0", psBlob);

	HRESULT hr2 = g_d3d.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_d3d.vs);
	if (FAILED(hr2)) throw std::runtime_error("CreateVertexShader failed");
	hr2 = g_d3d.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_d3d.ps);
	if (FAILED(hr2)) throw std::runtime_error("CreatePixelShader failed");

	D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	hr2 = g_d3d.device->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_d3d.inputLayout);
	if (FAILED(hr2)) throw std::runtime_error("CreateInputLayout failed");

	Vertex quad[4] = {
		{ -1.f, -1.f, 0.f, 1.f },
		{ -1.f,  1.f, 0.f, 0.f },
		{  1.f, -1.f, 1.f, 1.f },
		{  1.f,  1.f, 1.f, 0.f },
	};
	D3D11_BUFFER_DESC vbDesc{};
	vbDesc.ByteWidth = sizeof(quad);
	vbDesc.Usage = D3D11_USAGE_DEFAULT;
	vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

	D3D11_SUBRESOURCE_DATA vbData{};
	vbData.pSysMem = quad;
	hr2 = g_d3d.device->CreateBuffer(&vbDesc, &vbData, &g_d3d.vertexBuffer);
	if (FAILED(hr2)) throw std::runtime_error("CreateBuffer vertexBuffer failed");

	struct ScaleCB {
		float scaleX;
		float scaleY;
		float pad[2];
	};
	ScaleCB cbInit{ 1.f, 1.f, {0.f,0.f} };
	D3D11_BUFFER_DESC cbDesc{};
	cbDesc.ByteWidth = sizeof(ScaleCB);
	cbDesc.Usage = D3D11_USAGE_DEFAULT;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	D3D11_SUBRESOURCE_DATA cbData{};
	cbData.pSysMem = &cbInit;
	hr2 = g_d3d.device->CreateBuffer(&cbDesc, &cbData, &g_d3d.scaleCB);
	if (FAILED(hr2)) throw std::runtime_error("CreateBuffer scaleCB failed");

	D3D11_SAMPLER_DESC sampDesc{};
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampDesc.MinLOD = 0;
	sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

	hr2 = g_d3d.device->CreateSamplerState(&sampDesc, &g_d3d.sampler);
	if (FAILED(hr2)) throw std::runtime_error("CreateSamplerState failed");

	g_d3d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	D3D11_VIEWPORT vp{};
	vp.TopLeftX = 0.f;
	vp.TopLeftY = 0.f;
	vp.Width = (FLOAT)g_clientW;
	vp.Height = (FLOAT)g_clientH;
	vp.MinDepth = 0.f;
	vp.MaxDepth = 1.f;
	g_d3d.context->RSSetViewports(1, &vp);
}

static void ResizeD3D(HWND hwnd) {
	if (!g_d3d.swapChain || !g_d3d.device || !g_d3d.context) return;

	RECT rc{};
	if (!GetClientRect(hwnd, &rc)) return;
	int w = rc.right - rc.left;
	int h = rc.bottom - rc.top;
	if (w <= 0 || h <= 0) return;
	if (w == g_clientW && h == g_clientH) return;

	g_d3d.context->OMSetRenderTargets(0, nullptr, nullptr);
	g_d3d.rtv.Reset();

	HRESULT hr = g_d3d.swapChain->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr)) {
		Logf("[d3d] ResizeBuffers failed: 0x%08X\n", (unsigned)hr);
		return;
	}

	ComPtr<ID3D11Texture2D> backBuffer;
	hr = g_d3d.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
	if (FAILED(hr)) {
		Logf("[d3d] GetBuffer after resize failed: 0x%08X\n", (unsigned)hr);
		return;
	}

	hr = g_d3d.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_d3d.rtv);
	if (FAILED(hr)) {
		Logf("[d3d] CreateRenderTargetView after resize failed: 0x%08X\n", (unsigned)hr);
		return;
	}

	g_clientW = w;
	g_clientH = h;

	D3D11_VIEWPORT vp{};
	vp.TopLeftX = 0.f;
	vp.TopLeftY = 0.f;
	vp.Width = (FLOAT)g_clientW;
	vp.Height = (FLOAT)g_clientH;
	vp.MinDepth = 0.f;
	vp.MaxDepth = 1.f;
	g_d3d.context->RSSetViewports(1, &vp);

	UpdateLetterboxRect();
}

static void EnsureTextureIfNeeded(int w, int h) {
	if (w <= 0 || h <= 0) return;
	if (g_d3d.texture && g_d3d.textureSRV) {
		// We assume resolution doesn't change often; keep it simple.
		// For safety, recreate when size differs.
		D3D11_TEXTURE2D_DESC td{};
		g_d3d.texture->GetDesc(&td); // GetDesc returns void (no HRESULT)
		if ((int)td.Width == w && (int)td.Height == h) return;
	}
	g_d3d.textureSRV.Reset();
	g_d3d.texture.Reset();

	D3D11_TEXTURE2D_DESC td{};
	td.Width = (UINT)w;
	td.Height = (UINT)h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = 0;

	HRESULT hr = g_d3d.device->CreateTexture2D(&td, nullptr, &g_d3d.texture);
	if (FAILED(hr)) throw std::runtime_error("CreateTexture2D failed");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = td.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	hr = g_d3d.device->CreateShaderResourceView(g_d3d.texture.Get(), &srvDesc, &g_d3d.textureSRV);
	if (FAILED(hr)) throw std::runtime_error("CreateShaderResourceView failed");
}

static void RenderOneFrame(int vw, int vh) {
	(void)vw;
	(void)vh;

	float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
	ID3D11RenderTargetView* rtvs[] = { g_d3d.rtv.Get() };
	g_d3d.context->OMSetRenderTargets(1, rtvs, nullptr);
	g_d3d.context->ClearRenderTargetView(g_d3d.rtv.Get(), clearColor);

	// Letterbox scaling: keep it consistent with hit-test rect.
	if (vw > 0 && vh > 0 && g_clientW > 0 && g_clientH > 0 && g_d3d.textureSRV) {
		const double winAspect = (double)g_clientW / (double)g_clientH;
		const double vidAspect = (double)vw / (double)vh;

		double scaleX = 1.0;
		double scaleY = 1.0;
		if (winAspect >= vidAspect) {
			scaleX = vidAspect / winAspect;
			scaleY = 1.0;
		} else {
			scaleX = 1.0;
			scaleY = winAspect / vidAspect;
		}

		struct ScaleCB {
			float scaleX;
			float scaleY;
			float pad[2];
		};
		ScaleCB cb{};
		cb.scaleX = (float)scaleX;
		cb.scaleY = (float)scaleY;
		g_d3d.context->UpdateSubresource(g_d3d.scaleCB.Get(), 0, nullptr, &cb, 0, 0);

		UINT stride = sizeof(Vertex);
		UINT offset = 0;
		g_d3d.context->IASetInputLayout(g_d3d.inputLayout.Get());
		g_d3d.context->IASetVertexBuffers(0, 1, g_d3d.vertexBuffer.GetAddressOf(), &stride, &offset);

		g_d3d.context->VSSetShader(g_d3d.vs.Get(), nullptr, 0);
		g_d3d.context->PSSetShader(g_d3d.ps.Get(), nullptr, 0);

		ID3D11Buffer* cbs[] = { g_d3d.scaleCB.Get() };
		ID3D11ShaderResourceView* srvs[] = { g_d3d.textureSRV.Get() };
		ID3D11SamplerState* samps[] = { g_d3d.sampler.Get() };
		g_d3d.context->VSSetConstantBuffers(0, 1, cbs);
		g_d3d.context->PSSetShaderResources(0, 1, srvs);
		g_d3d.context->PSSetSamplers(0, 1, samps);

		g_d3d.context->Draw(4, 0);
	}
}

static void StartLatencyPingThread() {
	if (g_pingThreadStarted.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	// Send latPing periodically to compute theta.
	std::thread([] {
		uint64_t pingSeq = 0;
		while (!g_exitRequested.load(std::memory_order_relaxed)) {
			std::shared_ptr<rtc::DataChannel> dc;
			{
				std::lock_guard<std::recursive_mutex> lk(g_dcMtx);
				dc = g_dataChannel;
			}
			if (!dc || !dc->isOpen()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}

			const uint64_t tCli = SystemMsNow();
			pingSeq++;
			json j = { {"type", "latPing"}, {"seq", pingSeq}, {"tCli", (int64_t)tCli} };
			// Queue send to avoid any possible re-entrancy/deadlock inside libdatachannel.
			SendToDataChannelAsync(j.dump());

			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
		}
		g_pingThreadStarted.store(false, std::memory_order_release);
	}).detach();
}

static void HandleDataChannelMessage(const std::string& msg) {
	// Expected JSON messages from server: frameMark / latPong / controlGranted etc.
	// Also handle Ping from server for compatibility with existing ping-pong logic.
	if (msg.find("Ping") != std::string::npos) {
		// Avoid being spammed: only reply to Ping every 10 seconds.
		// This keeps logs readable and prevents Ping/Pong loops from flooding.
		static std::atomic<uint64_t> lastPongMs{0};
		const uint64_t now = SystemMsNow();
		uint64_t expected = lastPongMs.load(std::memory_order_relaxed);
		if (expected == 0 || (now >= expected + 10000)) {
			if (lastPongMs.compare_exchange_strong(
				expected, now,
				std::memory_order_relaxed, std::memory_order_relaxed)) {
				SendToDataChannelAsync("Pong " + std::to_string(now));
			}
		}
		return;
	}

	json j;
	try {
		j = json::parse(msg);
	} catch (...) {
		return;
	}

	const std::string type = j.value("type", "");
	if (type == "latPong") {
		const uint64_t tCli = (uint64_t)j.value("tCli", (int64_t)0);
		const uint64_t tSrv = (uint64_t)j.value("tSrv", (int64_t)0);
		const uint64_t t2 = SystemMsNow();
		const uint64_t rtt = (t2 >= tCli) ? (t2 - tCli) : 0;
		const double theta = ((double)tSrv + (double)rtt / 2.0) - (double)t2;
		{
			std::lock_guard<std::mutex> lk(g_latency.mtx);
			SmoothEwma(g_latency.thetaMs, theta, 0.25);
		}
		return;
	}

	if (type == "frameMark") {
		const uint64_t seq = (uint64_t)j.value("seq", (uint64_t)0);
		const uint64_t srvMs = (uint64_t)j.value("srvMs", (uint64_t)0);
		{
			std::lock_guard<std::mutex> lk(g_latency.mtx);
			g_latency.srvMsBySeq[seq] = srvMs;
			g_latency.lastFrameMarkSeq = seq;
			g_latency.lastFrameMarkSrvMs = srvMs;
			g_latency.hasLastFrameMark = true;
			g_latency.prune(seq);
		}
		return;
	}

	if (type == "controlGranted") {
		g_controlEnabled.store(true, std::memory_order_relaxed);
		return;
	}
	if (type == "controlDenied" || type == "controlRevoked") {
		g_controlEnabled.store(false, std::memory_order_relaxed);
		return;
	}

	if (type == "remoteProcessExited") {
		Logf("[receiver] remoteProcessExited received\n");
		g_exitRequested.store(true, std::memory_order_relaxed);
		g_inputArmed.store(false, std::memory_order_relaxed);
		if (g_hwnd) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
		return;
	}
}

static void SetupDataChannelCallbacks(const std::shared_ptr<rtc::PeerConnection>& pc) {
	pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
		{
			std::lock_guard<std::recursive_mutex> lk(g_dcMtx);
			g_dataChannel = dc;
		}

		dc->onOpen([dc] {
			(void)dc;
			g_controlEnabled.store(false, std::memory_order_relaxed);

			// Request control (server will grant/deny based on controller limiter).
			json req = { {"type", "controlRequest"} };
			SendToDataChannelAsync(req.dump());

			StartLatencyPingThread();
		});

		dc->onMessage(nullptr, [](std::string msg) {
			HandleDataChannelMessage(msg);
		});

		dc->onClosed([] {
			g_controlEnabled.store(false, std::memory_order_relaxed);
			g_videoTrackAttached.store(false, std::memory_order_relaxed);
			std::lock_guard<std::recursive_mutex> lk(g_dcMtx);
			g_dataChannel.reset();
			g_videoTrackKeepAlive.reset();
		});
	});
}

static std::shared_ptr<rtc::PeerConnection> SetupPeerConnectionForAnswerer(
	const std::shared_ptr<rtc::WebSocket>& ws,
	std::atomic<bool>& offerHandled,
	H264AnnexBDecoderToBGRA& decoder
) {
	rtc::Configuration config;
	config.disableAutoNegotiation = true;
	config.iceServers.emplace_back("stun:stun.l.google.com:19302");

	auto pc = std::make_shared<rtc::PeerConnection>(config);
	std::weak_ptr<rtc::PeerConnection> wpc = pc;

	pc->onGatheringStateChange([wpc, &offerHandled](rtc::PeerConnection::GatheringState st) {
		if (st != rtc::PeerConnection::GatheringState::Complete) return;
		Logf("[signaling] gathering complete\n");
		if (offerHandled.load(std::memory_order_relaxed)) return; // send once
		auto pc2 = wpc.lock();
		if (!pc2) {
			Logf("[signaling] gathering complete but pc expired\n");
			return;
		}
		auto ldOpt = pc2->localDescription();
		if (!ldOpt.has_value()) {
			Logf("[signaling] gathering complete but no localDescription\n");
			return;
		}
		const rtc::Description ld = ldOpt.value();
		{
			// Diagnostics: check if SDP contains a non-zero m=video port.
			int vport = -1;
			const std::string sdpTmp = std::string(ld);
			auto pos = sdpTmp.find("m=video ");
			if (pos != std::string::npos) {
				size_t i = pos + std::strlen("m=video ");
				while (i < sdpTmp.size() && (sdpTmp[i] < '0' || sdpTmp[i] > '9')) ++i;
				if (i < sdpTmp.size()) {
					size_t j = i;
					while (j < sdpTmp.size() && (sdpTmp[j] >= '0' && sdpTmp[j] <= '9')) ++j;
					if (j > i) vport = std::atoi(sdpTmp.substr(i, j - i).c_str());
				}
			}
			Logf("[signaling] gathering complete localDescription ended=%d m=video port=%d\n",
				(int)ld.ended(), vport);
		}
		std::string sdp = std::string(ld); // include candidates (non-trickle signaling)
		// Diagnose candidate count (helps verify ICE is present in SDP).
		size_t candCnt = 0;
		for (size_t p = sdp.find("a=candidate:"); p != std::string::npos; p = sdp.find("a=candidate:", p + 1)) {
			++candCnt;
		}
		// libdatachannel may output `m=<media> 0` in the gathered localDescription.
		// Some interop paths treat port=0 as "disabled", preventing RTP setup.
		// Use dummy port 9 (common WebRTC convention) for recv-only tracks.
		auto FixRecvOnlyMLinePort = [&](const char* media) {
			const std::string from = std::string("m=") + media + " 0 ";
			const std::string to = std::string("m=") + media + " 9 ";
			size_t pos = 0;
			bool changed = false;
			while ((pos = sdp.find(from, pos)) != std::string::npos) {
				sdp.replace(pos, from.size(), to);
				pos += to.size();
				changed = true;
			}
			return changed;
		};

		const bool chV = FixRecvOnlyMLinePort("video");
		const bool chA = FixRecvOnlyMLinePort("audio");
		if (chV) Logf("[signaling] fixed SDP m=video 0 -> 9 (candidates=%llu)\n",
			(unsigned long long)candCnt);
		if (chA) Logf("[signaling] fixed SDP m=audio 0 -> 9 (candidates=%llu)\n",
			(unsigned long long)candCnt);
		if (!chV && !chA) {
			Logf("[signaling] SDP m-line ports already non-zero (candidates=%llu)\n",
				(unsigned long long)candCnt);
		}

		Logf("[signaling] sending SDP (candidates=%llu)\n",
			(unsigned long long)candCnt);

		// Always send `type=answer` for receiver -> offerer signaling.
		json ans = { {"id", "server"}, {"type", "answer"}, {"sdp", sdp} };
		try {
			const std::string payload = ans.dump();
			{
				std::lock_guard<std::mutex> lk(g_wsSendPendingMtx);
				g_wsSendPending.push(payload);
			}
			Logf("[signaling] queued answer payload (gathering complete) bytes=%llu\n",
				(unsigned long long)payload.size());
			offerHandled.store(true, std::memory_order_relaxed);
		} catch (...) {
			Logf("[signaling] queue answer payload failed\n");
		}
	});

	pc->onLocalDescription([&](rtc::Description ld) {
		// Diagnostics only: sending is done when gathering completes, because
		// libdatachannel may call onLocalDescription before candidates are ended,
		// and may not call it again when ended becomes true.
		static std::atomic<int> ldCnt{0};
		const int c = ldCnt.fetch_add(1, std::memory_order_relaxed);
		if (c < 5) {
			Logf("[signaling] onLocalDescription type=%s ended=%d\n",
				ld.typeString().c_str(), (int)ld.ended());
		}
	});

	pc->onTrack([&](std::shared_ptr<rtc::Track> track) {
		if (!track) return;
		const std::string mid = track->mid();
		const std::string mediaType = track->description().type();
		Logf("[receiver] onTrack mid=%s mediaType=%s\n", mid.c_str(), mediaType.c_str());
		// Only attach H264 depacketizer for video track.
		if (mediaType != "video") {
			Logf("[receiver] onTrack ignored (not video)\n");
			return;
		}
		if (g_videoTrackAttached.exchange(true, std::memory_order_acq_rel)) return;
		Logf("[receiver] onVideoTrack attached\n");

		// Keep the track object alive; otherwise libdatachannel may destroy it
		// right after onTrack callback, preventing onFrame from firing.
		g_videoTrackKeepAlive = track;

		auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
			// Output AnnexB with 0x00000001 start codes (valid for FFmpeg AnnexB decoder).
			rtc::H264RtpDepacketizer::Separator::LongStartSequence);
		try {
			track->setMediaHandler(depacketizer);
		} catch (const std::exception& e) {
			Logf("[receiver] setMediaHandler failed: %s\n", e.what());
			return;
		} catch (...) {
			Logf("[receiver] setMediaHandler failed: unknown\n");
			return;
		}

		track->onFrame([&decoder](rtc::binary data, rtc::FrameInfo /*info*/) {
			static std::atomic<uint64_t> frameCbCnt{0};
			static std::atomic<uint64_t> emptyCbCnt{0};

			const uint64_t cbn = frameCbCnt.fetch_add(1, std::memory_order_relaxed);
			const bool isEmpty = data.empty();

			if (isEmpty) {
				const uint64_t en = emptyCbCnt.fetch_add(1, std::memory_order_relaxed);
				if (en < 5) {
					Logf("[receiver] onFrame callback but data EMPTY (cbn=%llu)\n", (unsigned long long)cbn);
				}
				return;
			}

			if (cbn < 5) {
				Logf("[receiver] onFrame got rtc::binary size=%llu\n", (unsigned long long)data.size());
				// Dump a small prefix to help validate whether this looks like AnnexB.
				const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
				char hexbuf[80]{};
				const size_t dumpN = (data.size() < 16) ? data.size() : 16;
				for (size_t i = 0; i < dumpN && (i * 3 + 2) < sizeof(hexbuf); ++i) {
					// Each byte: "XX "
					std::snprintf(hexbuf + i * 3, 4, "%02X ", p[i]);
				}
				Logf("[receiver] onFrame prefix=%s\n", hexbuf);
			}

			const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
			const size_t n = data.size();

			auto looksLikeAnnexB = [&]() -> bool {
				if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true; // 00 00 00 01
				if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return true; // 00 00 01
				return false;
			};

			try {
				bool decoded = decoder.decodeAnnexB(p, n);
				if (!decoded && !looksLikeAnnexB()) {
					// Try length-prefixed NALUs: [uint32_be len][nalu bytes]...
					// Convert to AnnexB start codes and decode again.
					std::vector<uint8_t> annexb;
					annexb.reserve(n + (n / 4) * 4);
					size_t off = 0;
					bool ok = true;
					while (off + 4 <= n) {
						const uint32_t len =
							(uint32_t(p[off + 0]) << 24) |
							(uint32_t(p[off + 1]) << 16) |
							(uint32_t(p[off + 2]) << 8) |
							(uint32_t(p[off + 3]) << 0);
						off += 4;
						if (len == 0 || off + size_t(len) > n) { ok = false; break; }
						// AnnexB 4-byte start code
						annexb.insert(annexb.end(), { 0x00, 0x00, 0x00, 0x01 });
						annexb.insert(annexb.end(), p + off, p + off + len);
						off += size_t(len);
					}
					if (ok && !annexb.empty()) {
						decoded = decoder.decodeAnnexB(annexb.data(), annexb.size());
						if (decoded && cbn < 5) {
							Logf("[receiver] decodeAnnexB failed; length->AnnexB decode ok\n");
						}
					}
				}

				if (decoded) {
					static std::atomic<uint64_t> lastLogMs{0};
					const uint64_t now = SystemMsNow();
					uint64_t prev = lastLogMs.load(std::memory_order_relaxed);
					if (now - prev > 1000) {
						if (lastLogMs.compare_exchange_strong(prev, now, std::memory_order_relaxed)) {
							Logf("[receiver] decodeAnnexB ok (throttled)\n");
						}
					}
				}
			} catch (const std::exception& e) {
				if (cbn < 5) Logf("[receiver] decode exception: %s\n", e.what());
			} catch (...) {
				if (cbn < 5) Logf("[receiver] decode exception: unknown\n");
			}
		});
	});

	SetupDataChannelCallbacks(pc);
	return pc;
}

static void SendRequestOnWebSocketOpen(const std::shared_ptr<rtc::WebSocket>& ws,
	const std::string& exePath) {
	json req = { {"id", "server"}, {"type", "request"} };
	if (!exePath.empty()) req["exePath"] = exePath;
	ws->send(req.dump());
}

static void RunSignalingAndWebRtc(
	const std::string& host,
	int port,
	const std::string& clientId,
	const std::string& exePath
) {
	EnsureWinsockOnce();

	auto ws = std::make_shared<rtc::WebSocket>();
	std::atomic<bool> offerHandled{false};
	std::atomic<bool> wsOpened{false};
	std::atomic<bool> requestSent{false};

	// Copy for async callbacks (avoid dangling refs if stack/layout changes).
	const std::string exePathLocal = exePath;

	const std::string requestPayload = [exePathLocal] {
		json req = { {"id", "server"}, {"type", "request"} };
		if (!exePathLocal.empty()) req["exePath"] = exePathLocal;
		return req.dump();
	}();

	ws->onOpen([ws, exePathLocal, &wsOpened] {
		Logf("[signaling] ws onOpen fired\n");
		wsOpened.store(true, std::memory_order_relaxed);
	});

	ws->onError([](const std::string& err) {
		Logf("[signaling] WebSocket error: %s\n", err.c_str());
	});
	ws->onClosed([] {
		Logf("[signaling] WebSocket closed\n");
	});

	std::mutex pcMtx;
	std::shared_ptr<rtc::PeerConnection> pc;
	// FFmpeg/libav init can interact badly with some Winsock/libdatachannel init orders on Windows;
	// create decoder only when we actually need it (first offer), after ws->open.
	std::shared_ptr<H264AnnexBDecoderToBGRA> decoder;

	ws->onMessage([&](const std::variant<rtc::binary, std::string>& data) {
		if (std::holds_alternative<std::string>(data)) {
			const std::string& str = std::get<std::string>(data);
			json msg;
			try { msg = json::parse(str); }
			catch (...) {
				static std::atomic<int> parseFailCnt{0};
				int c = parseFailCnt.fetch_add(1, std::memory_order_relaxed);
				if (c < 5) Logf("[signaling] onMessage JSON parse failed\n");
				return;
			}

			const std::string type = msg.value("type", "");
			static std::atomic<int> recvMsgCnt{0};
			int rc = recvMsgCnt.fetch_add(1, std::memory_order_relaxed);
			if (rc < 20) Logf("[signaling] onMessage type=%s\n", type.c_str());
			if (type == "offer") {
				const std::string sdp = msg.value("sdp", "");

				// Only handle first offer.
				std::lock_guard<std::mutex> lk(pcMtx);
				if (!pc) {
					if (!decoder) {
						try {
							decoder = std::make_shared<H264AnnexBDecoderToBGRA>();
						} catch (const std::exception& e) {
							Logf("[decoder] FFmpeg init failed: %s\n", e.what());
							return;
						}
					}
					pc = SetupPeerConnectionForAnswerer(ws, offerHandled, *decoder);

					rtc::Description offer(sdp, type);
					if (pc) {
						pc->setRemoteDescription(offer);
						pc->setLocalDescription(rtc::Description::Type::Answer);
						// Ensure candidates are gathered so SDP answer contains media ports.
						pc->gatherLocalCandidates();
					}
				}
			}
		}
		});

	// Signaling relay server requires local connection id in path: /clientId
	
	const std::string url = "ws://" + host + ":" + std::to_string(port) + "/" + clientId;
	std::printf("[signaling] ws url: %s\n", url.c_str());
	try {
		ws->open(url);
	} catch (const std::exception& e) {
		Logf("[signaling] ws->open failed: %s\n", e.what());
		throw;
	} catch (...) {
		Logf("[signaling] ws->open failed: unknown exception\n");
		throw;
	}

	// Make ws available for queued sends (answer payload, etc.)
	{
		std::lock_guard<std::mutex> lk(g_wsMtx);
		g_wsForRequest = ws;
	}

	// Wait until exit requested or websocket closed.
	Logf("[signaling] ws->open returned, isOpen=%d isClosed=%d\n",
		ws ? (int)ws->isOpen() : 0,
		ws ? (int)ws->isClosed() : 1);

	static std::atomic<int> loopLogCnt{0};
	while (!g_exitRequested.load(std::memory_order_relaxed) && ws && !ws->isClosed()) {
		const int lc = loopLogCnt.fetch_add(1, std::memory_order_relaxed);
		if (lc < 5) {
			Logf("[signaling] loop iter=%d wsOpened=%d requestSent=%d isOpen=%d\n",
				lc,
				(int)wsOpened.load(std::memory_order_relaxed),
				(int)requestSent.load(std::memory_order_relaxed),
				(int)ws->isOpen());
		}
		// Now that onOpen has fired, send request once from the signaling thread.
		if (wsOpened.load(std::memory_order_relaxed) && !requestSent.load(std::memory_order_relaxed)) {
			try {
				Logf("[signaling] sending request from signaling thread\n");
				ws->send(requestPayload);
				requestSent.store(true, std::memory_order_relaxed);
				Logf("[signaling] request sent ok\n");
			} catch (const std::exception& e) {
				Logf("[signaling] ws->send request failed: %s\n", e.what());
				requestSent.store(true, std::memory_order_relaxed); // avoid retry storm
			} catch (...) {
				Logf("[signaling] ws->send request failed: unknown\n");
				requestSent.store(true, std::memory_order_relaxed);
			}
		}

		DrainWsSendPending();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
}

static void ApplyFullscreenAllMonitors(HWND hwnd) {
	const int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	const int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
	// One-time Z-order bump; avoid repeatedly stealing focus every frame.
	SetWindowPos(hwnd, HWND_TOP, sx, sy, sw, sh, SWP_SHOWWINDOW);
}

static void ApplyFullscreenPrimary(HWND hwnd) {
	MONITORINFOEXW mi{};
	mi.cbSize = sizeof(mi);
	HMONITOR hMon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
	if (!hMon || !GetMonitorInfoW(hMon, &mi)) {
		const int sw = GetSystemMetrics(SM_CXSCREEN);
		const int sh = GetSystemMetrics(SM_CYSCREEN);
		SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(hwnd, HWND_TOP, 0, 0, sw, sh, SWP_SHOWWINDOW);
		return;
	}
	const RECT& r = mi.rcMonitor;
	const int w = r.right - r.left;
	const int h = r.bottom - r.top;
	SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
	SetWindowPos(hwnd, HWND_TOP, r.left, r.top, w, h, SWP_SHOWWINDOW);
}

static void ResizeWindowToVideoResolution(HWND hwnd, int videoW, int videoH) {
	if (!hwnd || !g_windowed || videoW <= 0 || videoH <= 0) return;

	static int s_lastAppliedVideoW = 0;
	static int s_lastAppliedVideoH = 0;
	if (s_lastAppliedVideoW == videoW && s_lastAppliedVideoH == videoH) return;

	RECT work{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const int workW = (std::max)(1, (int)(work.right - work.left));
	const int workH = (std::max)(1, (int)(work.bottom - work.top));

	// Windowed mode: follow incoming video resolution while keeping window inside work area.
	double scale = 1.0;
	if (videoW > workW || videoH > workH) {
		const double sx = (double)workW / (double)videoW;
		const double sy = (double)workH / (double)videoH;
		scale = (std::min)(sx, sy);
	}
	const int targetClientW = (std::max)(320, (int)std::round((double)videoW * scale));
	const int targetClientH = (std::max)(180, (int)std::round((double)videoH * scale));

	RECT rc{ 0, 0, targetClientW, targetClientH };
	AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
	const int winW = rc.right - rc.left;
	const int winH = rc.bottom - rc.top;

	RECT cur{};
	GetWindowRect(hwnd, &cur);
	const int curW = cur.right - cur.left;
	const int curH = cur.bottom - cur.top;
	if (curW == winW && curH == winH) {
		s_lastAppliedVideoW = videoW;
		s_lastAppliedVideoH = videoH;
		return;
	}

	const int x = work.left + (workW - winW) / 2;
	const int y = work.top + (workH - winH) / 2;
	SetWindowPos(hwnd, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
	s_lastAppliedVideoW = videoW;
	s_lastAppliedVideoH = videoH;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
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

		// JS behavior: send mouseMove first, then mouseDown.
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
			ResizeD3D(hwnd);
		return 0;
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

int main(int argc, char** argv) {
	std::string host = "127.0.0.1";
	int port = 9090;
	std::string clientId;
	std::string exePath;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--host" && i + 1 < argc) host = argv[++i];
		else if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
		else if (arg == "--clientId" && i + 1 < argc) clientId = argv[++i];
		else if (arg == "--exePath" && i + 1 < argc) exePath = argv[++i];
		else if (arg == "--windowed") g_windowed = true;
		else if (arg == "--fullscreen") g_windowed = false;
		else if (arg == "--allMonitors") g_fullscreenAllMonitors = true;
		else if (arg == "--maxFps" && i + 1 < argc) g_maxPresentFps = std::atoi(argv[++i]);
		else if (arg == "--help" || arg == "-h") {
			std::printf(
				"Usage:\n"
				"  cpp_receiver.exe --host <host> [--port 9090] --exePath <path> [--clientId <id>]\n"
				"Display / performance:\n"
				"  (default)           Windowed mode; use taskbar / other apps normally\n"
				"  --fullscreen        Borderless fullscreen (primary or --allMonitors)\n"
				"  --windowed          Same as default (explicit)\n"
				"  --allMonitors       With --fullscreen: cover all monitors\n"
				"  --maxFps <n>        Cap present rate (default 60; 0 = unlimited)\n"
				"Examples:\n"
				"  cpp_receiver.exe --host 127.0.0.1 --exePath \"C:\\\\Windows\\\\System32\\\\notepad.exe\"\n"
				"\n");
			return 0;
		}
	}

	if (clientId.empty()) clientId = RandomId(10);
	InitLogFile(clientId);
	// Ensure access violations also get logged.
	SetUnhandledExceptionFilter(UnhandledExceptionLogger);
	std::printf("[receiver] clientId=%s\n", clientId.c_str());

	HINSTANCE hInstance = GetModuleHandle(nullptr);
	const wchar_t* className = L"cpp_receiver_window";
	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
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
			0, className, L"cpp_receiver",
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
			0, className, L"cpp_receiver",
			WS_POPUP | WS_VISIBLE, sx, sy, sw, sh,
			nullptr, nullptr, hInstance, nullptr);
		if (g_fullscreenAllMonitors)
			ApplyFullscreenAllMonitors(g_hwnd);
		else
			ApplyFullscreenPrimary(g_hwnd);
	}
	if (!g_hwnd) {
		Logf("CreateWindowExW failed\n");
		return 1;
	}
	ShowWindow(g_hwnd, SW_SHOW);
	UpdateWindow(g_hwnd);

	// Initialize timing base (QPC -> system epoch ms).
	LARGE_INTEGER freq{};
	LARGE_INTEGER qpc0{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&qpc0);
	g_qpcFreq = (uint64_t)freq.QuadPart;
	g_baseQpc = (uint64_t)qpc0.QuadPart;
	g_baseSysMs = SystemMsNow();

	try {
		// Increase verbosity to catch media-handler exceptions (RTP depacketizer).
		rtc::InitLogger(rtc::LogLevel::Info);
		EnsureD3D(g_hwnd);
		UpdateLetterboxRect();
	} catch (const std::exception& e) {
		Logf("Init D3D failed: %s\n", e.what());
		return 2;
	}

	// Run signaling/WebRTC in background thread.
	std::thread t([&] {
		try {
			RunSignalingAndWebRtc(host, port, clientId, exePath);
		} catch (const std::exception& e) {
			Logf("WebRTC thread error: %s\n", e.what());
			g_exitRequested.store(true, std::memory_order_relaxed);
			PostMessage(g_hwnd, WM_CLOSE, 0, 0);
		}
	});
	t.detach();

	// Render loop: present when a decoded frame is ready.
	// For phase1: map displayed frames to frameMark seq by ordered assumption.
	std::optional<uint64_t> baseSeq;
	uint64_t displayedFrameCount = 0;
	uint64_t lastShownDecodedIndex = 0;

	std::vector<uint8_t> localBgra;

	MSG msg{};
	static uint64_t g_debugPrintedFrames = 0;
	while (!g_exitRequested.load(std::memory_order_relaxed)) {
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		if (g_exitRequested.load(std::memory_order_relaxed)) break;

		// Send queued DataChannel messages on the main/render thread.
		DrainDcSendPending();

		bool haveFrame = false;
		int w = 0;
		int h = 0;
		uint64_t decodedIndex = 0;
		{
			std::lock_guard<std::mutex> lk(g_frameMtx);
			if (g_sharedFrame.ready && g_sharedFrame.decodedIndex != lastShownDecodedIndex) {
				haveFrame = true;
				w = g_sharedFrame.w;
				h = g_sharedFrame.h;
				decodedIndex = g_sharedFrame.decodedIndex;
				localBgra.swap(g_sharedFrame.bgra);
				g_sharedFrame.ready = false;
				lastShownDecodedIndex = decodedIndex;
			}
		}

		if (!haveFrame) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		if (g_debugPrintedFrames < 5) {
			Logf("[receiver] show frame idx=%llu size=%dx%d\n",
				(unsigned long long)decodedIndex, w, h);
			++g_debugPrintedFrames;
		}
		ResizeWindowToVideoResolution(g_hwnd, w, h);

		// Publish texture update + draw.
		// Note: access violations may not be caught by C++ exceptions.
		// We install an unhandled exception logger at process level.
		const bool logStages = (decodedIndex <= 2);
		try {
			if (logStages) Logf("[receiver] stage EnsureTextureIfNeeded begin\n");
			EnsureTextureIfNeeded(w, h);
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
			UpdateLetterboxRectFor(w, h);

			// Draw
			if (logStages) Logf("[receiver] stage RenderOneFrame begin\n");
			RenderOneFrame(w, h);
			if (logStages) Logf("[receiver] stage RenderOneFrame end\n");

			// Present and time it (QPC immediately after Present, before optional FPS cap sleep)
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

		if (g_maxPresentFps > 0) {
			using namespace std::chrono;
			static steady_clock::time_point nextCap = steady_clock::now();
			// Defensive: avoid any possible divide-by-zero even if value gets corrupted.
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

		// Compute visible delay
		double thetaLocal = 0.0;
		bool hasTheta = false;
		uint64_t lastSeqLocal = 0;
		uint64_t lastSrvLocal = 0;
		{
			std::lock_guard<std::mutex> lk(g_latency.mtx);
			hasTheta = g_latency.thetaMs.has_value();
			thetaLocal = hasTheta ? g_latency.thetaMs.value() : 0.0;
			if (g_latency.hasLastFrameMark) {
				lastSeqLocal = g_latency.lastFrameMarkSeq;
				lastSrvLocal = g_latency.lastFrameMarkSrvMs;
			}
		}

		uint64_t srvMsUsed = 0;
		uint64_t visibleSeq = 0;
		if (!baseSeq.has_value() && g_latency.hasLastFrameMark) {
			baseSeq = lastSeqLocal;
			displayedFrameCount = 0;
		}
		if (baseSeq.has_value()) {
			visibleSeq = baseSeq.value() + displayedFrameCount;
			uint64_t srv = 0;
			bool found = false;
			{
				std::lock_guard<std::mutex> lk(g_latency.mtx);
				auto it = g_latency.srvMsBySeq.find(visibleSeq);
				if (it != g_latency.srvMsBySeq.end()) {
					srv = it->second;
					found = true;
				} else if (g_latency.hasLastFrameMark) {
					srv = g_latency.lastFrameMarkSrvMs;
					found = false;
				}
			}
			srvMsUsed = srv;
			(void)found;
		} else {
			srvMsUsed = lastSrvLocal;
			visibleSeq = lastSeqLocal;
		}

		double visibleDelayMs = hasTheta ? ((double)presentMs - (double)srvMsUsed + thetaLocal) : 0.0;

		wchar_t title[256]{};
		if (baseSeq.has_value() && hasTheta) {
			std::swprintf(
				title, 256, L"cpp_receiver | seq=%I64u delay~%.1fms (theta=%.1fms)",
				(unsigned __int64)visibleSeq, visibleDelayMs, thetaLocal);
		} else if (g_latency.hasLastFrameMark && hasTheta) {
			std::swprintf(
				title, 256, L"cpp_receiver | delay~%.1fms (theta=%.1fms)",
				visibleDelayMs, thetaLocal);
		} else {
			std::swprintf(title, 256, L"cpp_receiver | waiting latPong/align...");
		}
		SetWindowTextW(g_hwnd, title);

		displayedFrameCount++;
	}

	g_exitRequested.store(true, std::memory_order_relaxed);
	return 0;
}

