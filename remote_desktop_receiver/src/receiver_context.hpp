#pragma once

#include "rdr_win32_include.hpp"

#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>

#include <rtc/rtc.hpp>

using Microsoft::WRL::ComPtr;

#ifndef WM_RDR_NEW_VIDEO_FRAME
#define WM_RDR_NEW_VIDEO_FRAME (WM_APP + 64)
#endif

struct SharedVideoFrame {
	std::vector<uint8_t> bgra;
	int w = 0;
	int h = 0;
	uint64_t decodedIndex = 0;
	uint64_t frameId = 0;

	// Agent unix-ms timestamps embedded in H264 SEI for this specific displayed frame.
	uint64_t capMs = 0;
	uint64_t encMs = 0;
	uint64_t sendMs = 0;
	/// Agent: unix ms immediately before capture (grab) for this frame; SEI v2+ only.
	uint64_t prepMs = 0;
	bool hasPrepMs = false;
	bool hasAgentTimes = false;

	// Receiver-side absolute timestamps for pipeline segmenting.
	uint64_t rxMs = 0;        // SystemMsNow() when onFrame callback begins
	uint64_t decDoneMs = 0;  // SystemMsNow() when decode result is published into shared frame
	bool hasRxDecTimes = false;

	bool ready = false;
	std::chrono::steady_clock::time_point decodeQueuedSteady{};
	bool hasDecodeQueuedSteady = false;
};

struct LatencyState {
	std::mutex mtx;

	std::optional<double> thetaMs;
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

struct Vertex {
	float px;
	float py;
	float u;
	float v;
};

// --- shared process state (signaling / D3D / input / main loop) ---
extern std::atomic<bool> g_exitRequested;
extern HWND g_hwnd;
extern std::atomic<bool> g_pingThreadStarted;

extern bool g_windowed;
extern bool g_fullscreenAllMonitors;
extern int g_maxPresentFps;

extern std::mutex g_frameMtx;
extern SharedVideoFrame g_sharedFrame;

// Per-frame SEI injected via H264 (agent timestamps).
// onFrame extracts SEI and sets these before decode; publish() consumes them.
extern std::atomic<bool> g_pendingSeiValid;
extern std::atomic<uint64_t> g_pendingFrameId;
extern std::atomic<uint64_t> g_pendingCapMs;
extern std::atomic<uint64_t> g_pendingEncMs;
extern std::atomic<uint64_t> g_pendingSendMs;
extern std::atomic<uint64_t> g_pendingPrepMs;
extern std::atomic<bool> g_pendingHasPrepMs;

// Receiver rxMs captured at onFrame callback entry; consumed by publish().
extern std::atomic<bool> g_pendingRxMsValid;
extern std::atomic<uint64_t> g_pendingRxMs;

extern std::recursive_mutex g_dcMtx;
extern std::shared_ptr<rtc::DataChannel> g_dataChannel;
extern std::shared_ptr<rtc::Track> g_videoTrackKeepAlive;

extern std::mutex g_wsMtx;
extern std::shared_ptr<rtc::WebSocket> g_wsForRequest;
extern std::mutex g_wsSendPendingMtx;
extern std::queue<std::string> g_wsSendPending;

extern std::atomic<bool> g_controlEnabled;
extern std::atomic<bool> g_inputArmed;
extern std::atomic<bool> g_videoTrackAttached;
extern std::atomic<int> g_lastAbsX;
extern std::atomic<int> g_lastAbsY;

extern std::atomic<int> g_videoW;
extern std::atomic<int> g_videoH;
extern std::atomic<int> g_lbX;
extern std::atomic<int> g_lbY;
extern std::atomic<int> g_lbW;
extern std::atomic<int> g_lbH;

extern LatencyState g_latency;

extern D3DResources g_d3d;
extern int g_clientW;
extern int g_clientH;

extern uint64_t g_baseQpc;
extern uint64_t g_baseSysMs;
extern uint64_t g_qpcFreq;

uint64_t SystemMsNow();
uint64_t QpcToSysMs(uint64_t qpc);

void RdrNotifyNewVideoFrame();

bool SendToDataChannel(const std::string& s);
void SendToDataChannelAsync(std::string s);
void DrainDcSendPending();
void DrainWsSendPending();
