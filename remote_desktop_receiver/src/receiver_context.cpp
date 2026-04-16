#include "receiver_context.hpp"

#include "rdr_log.hpp"

#include <chrono>
#include <cstring>

std::atomic<bool> g_exitRequested{false};
HWND g_hwnd = nullptr;
std::atomic<bool> g_pingThreadStarted{false};

bool g_windowed = true;
bool g_fullscreenAllMonitors = false;
int g_maxPresentFps = 60;

std::mutex g_frameMtx;
SharedVideoFrame g_sharedFrame;

std::atomic<bool> g_pendingSeiValid{false};
std::atomic<uint64_t> g_pendingFrameId{0};
std::atomic<uint64_t> g_pendingCapMs{0};
std::atomic<uint64_t> g_pendingEncMs{0};
std::atomic<uint64_t> g_pendingSendMs{0};

std::atomic<bool> g_pendingRxMsValid{false};
std::atomic<uint64_t> g_pendingRxMs{0};

std::recursive_mutex g_dcMtx;
std::shared_ptr<rtc::DataChannel> g_dataChannel;
std::shared_ptr<rtc::Track> g_videoTrackKeepAlive;

std::mutex g_wsMtx;
std::shared_ptr<rtc::WebSocket> g_wsForRequest;
std::mutex g_wsSendPendingMtx;
std::queue<std::string> g_wsSendPending;

std::atomic<bool> g_controlEnabled{false};
std::atomic<bool> g_inputArmed{false};
std::atomic<bool> g_videoTrackAttached{false};
std::atomic<int> g_lastAbsX{-1};
std::atomic<int> g_lastAbsY{-1};

std::atomic<int> g_videoW{0};
std::atomic<int> g_videoH{0};
std::atomic<int> g_lbX{0};
std::atomic<int> g_lbY{0};
std::atomic<int> g_lbW{0};
std::atomic<int> g_lbH{0};

LatencyState g_latency;

D3DResources g_d3d;
int g_clientW = 0;
int g_clientH = 0;

uint64_t g_baseQpc = 0;
uint64_t g_baseSysMs = 0;
uint64_t g_qpcFreq = 0;

static std::recursive_mutex g_dcSendPendingMtx;
static std::queue<std::string> g_dcSendPending;

uint64_t SystemMsNow() {
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t QpcToSysMs(uint64_t qpc) {
	if (g_qpcFreq == 0) return 0;
	return g_baseSysMs + (uint64_t)(((double)(qpc - g_baseQpc) * 1000.0) / (double)g_qpcFreq);
}

void RdrNotifyNewVideoFrame() {
	HWND h = g_hwnd;
	if (!h || !IsWindow(h)) return;
	(void)PostMessageW(h, WM_RDR_NEW_VIDEO_FRAME, 0, 0);
}

bool SendToDataChannel(const std::string& s) {
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

void SendToDataChannelAsync(std::string s) {
	std::lock_guard<std::recursive_mutex> lk(g_dcSendPendingMtx);
	g_dcSendPending.push(std::move(s));
}

void DrainDcSendPending() {
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

void DrainWsSendPending() {
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
