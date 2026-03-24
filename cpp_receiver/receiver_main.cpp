#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

static std::atomic<bool> g_exitRequested{false};
static HWND g_hwnd = nullptr;
static std::atomic<bool> g_pingThreadStarted{false};

struct SharedVideoFrame {
	std::vector<uint8_t> bgra; // size = w*h*4
	int w = 0;
	int h = 0;
	uint64_t decodedIndex = 0;
	bool ready = false;
};

static std::mutex g_frameMtx;
static SharedVideoFrame g_sharedFrame;

static std::mutex g_dcMtx;
static std::shared_ptr<rtc::DataChannel> g_dataChannel;

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
	std::string out;
	out.resize(length);
	for (size_t i = 0; i < length; ++i) out[i] = chars[rand() % 61];
	return out;
}

static uint64_t SystemMsNow() {
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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
			std::fprintf(stderr, "[decoder] avcodec_send_packet failed ret=%d\n", ret);
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
		if (m_sws && outW == m_outW && outH == m_outH && srcFmt == m_srcFmt) return;

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
		std::lock_guard<std::mutex> lk(g_dcMtx);
		dc = g_dataChannel;
	}
	if (!dc || !dc->isOpen()) return false;
	try {
		return dc->send(s);
	} catch (...) {
		return false;
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
			if (errBlob) std::fprintf(stderr, "[d3d] shader compile error: %s\n", (const char*)errBlob->GetBufferPointer());
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
				std::lock_guard<std::mutex> lk(g_dcMtx);
				dc = g_dataChannel;
			}
			if (!dc || !dc->isOpen()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}

			const uint64_t tCli = SystemMsNow();
			pingSeq++;
			json j = { {"type", "latPing"}, {"seq", pingSeq}, {"tCli", (int64_t)tCli} };
			try {
				dc->send(j.dump());
			} catch (...) {
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
		}
		g_pingThreadStarted.store(false, std::memory_order_release);
	}).detach();
}

static void HandleDataChannelMessage(const std::string& msg) {
	// Expected JSON messages from server: frameMark / latPong / controlGranted etc.
	// Also handle Ping from server for compatibility with existing ping-pong logic.
	if (msg.find("Ping") != std::string::npos) {
		const uint64_t t = SystemMsNow();
		SendToDataChannel("Pong " + std::to_string(t));
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
		g_exitRequested.store(true, std::memory_order_relaxed);
		g_inputArmed.store(false, std::memory_order_relaxed);
		if (g_hwnd) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
		return;
	}
}

static void SetupDataChannelCallbacks(const std::shared_ptr<rtc::PeerConnection>& pc) {
	pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
		{
			std::lock_guard<std::mutex> lk(g_dcMtx);
			g_dataChannel = dc;
		}

		dc->onOpen([dc] {
			(void)dc;
			g_controlEnabled.store(false, std::memory_order_relaxed);

			// Request control (server will grant/deny based on controller limiter).
			json req = { {"type", "controlRequest"} };
			try { dc->send(req.dump()); } catch (...) {}

			StartLatencyPingThread();
		});

		dc->onMessage(nullptr, [](std::string msg) {
			HandleDataChannelMessage(msg);
		});

		dc->onClosed([] {
			g_controlEnabled.store(false, std::memory_order_relaxed);
			g_videoTrackAttached.store(false, std::memory_order_relaxed);
			std::lock_guard<std::mutex> lk(g_dcMtx);
			g_dataChannel.reset();
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

	pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState st) {
		if (st != rtc::PeerConnection::GatheringState::Complete) return;
		if (offerHandled.load(std::memory_order_relaxed)) return; // send answer once
		auto ldOpt = pc->localDescription();
		if (!ldOpt.has_value()) return;
		const rtc::Description ld = ldOpt.value();
		const std::string sdp = std::string(ld); // Description has operator string()
		json ans = { {"id", "server"}, {"type", ld.typeString()}, {"sdp", sdp} };
		try {
			ws->send(ans.dump());
		} catch (...) {
		}
		offerHandled.store(true, std::memory_order_relaxed);
	});

	pc->onTrack([&](std::shared_ptr<rtc::Track> track) {
		if (!track) return;
		if (g_videoTrackAttached.exchange(true, std::memory_order_acq_rel)) return;

		auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
			rtc::H264RtpDepacketizer::Separator::LongStartSequence);
		track->setMediaHandler(depacketizer);

		track->onFrame([&decoder](rtc::binary data, rtc::FrameInfo /*info*/) {
			if (data.empty()) return;
			const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
			decoder.decodeAnnexB(p, data.size());
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
	auto ws = std::make_shared<rtc::WebSocket>();
	H264AnnexBDecoderToBGRA decoder;
	std::atomic<bool> offerHandled{false};

	ws->onOpen([&ws, &exePath] {
		SendRequestOnWebSocketOpen(ws, exePath);
	});

	std::mutex pcMtx;
	std::shared_ptr<rtc::PeerConnection> pc;

	ws->onMessage([&](const std::variant<rtc::binary, std::string>& data) {
		if (std::holds_alternative<std::string>(data)) {
			const std::string& str = std::get<std::string>(data);
			json msg;
			try { msg = json::parse(str); }
			catch (...) { return; }

			const std::string type = msg.value("type", "");
			if (type == "offer") {
				const std::string sdp = msg.value("sdp", "");

				// Only handle first offer.
				std::lock_guard<std::mutex> lk(pcMtx);
				if (!pc) {
					// Need pc created before setRemoteDescription.
					pc = SetupPeerConnectionForAnswerer(ws, offerHandled, decoder);

					// Register track/data handlers before remote description is set.
					// Then set remote offer and generate local answer.
					rtc::Description offer(sdp, type);
					if (pc) { // <--- Add this check
						pc->setRemoteDescription(offer);
						pc->setLocalDescription(rtc::Description::Type::Answer);
					}
				}
				else {
					// If received again, ignore in phase1.
				}
			}
		}
		});


	// 信令服务端（Qt signaling relay）要求路径携带本地连接 id：/clientId
	const std::string url = "ws://" + host + ":" + std::to_string(port) + "/" + clientId;
	std::printf("[signaling] ws url: %s\n", url.c_str());
	try {
		ws->open(url);
	} catch (const std::exception& e) {
		std::fprintf(stderr, "[signaling] ws->open failed: %s\n", e.what());
		throw;
	} catch (...) {
		std::fprintf(stderr, "[signaling] ws->open failed: unknown exception\n");
		throw;
	}

	// Wait until exit requested or websocket closed.
	while (!g_exitRequested.load(std::memory_order_relaxed) && ws && ws->isOpen()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
}

static void EnsureWindowSizeForFullscreen(HWND hwnd) {
	const int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	const int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
	SetWindowPos(hwnd, HWND_TOP, sx, sy, sw, sh, SWP_SHOWWINDOW);
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
		SendToDataChannel(j.dump());
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
		SendToDataChannel(j.dump());
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
		SendToDataChannel(j.dump());
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
		SendToDataChannel(mv.dump());

		json j = { {"type", "mouseDown"}, {"button", 0}, {"x", 0}, {"y", 0} };
		SendToDataChannel(j.dump());
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
				SendToDataChannel(mv.dump());
			}
		}

		json j = { {"type", "mouseUp"}, {"button", 0}, {"x", 0}, {"y", 0} };
		SendToDataChannel(j.dump());
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
		SendToDataChannel(j.dump());
		return 0;
	}
	case WM_KILLFOCUS:
		g_inputArmed.store(false, std::memory_order_relaxed);
		g_lastAbsX.store(-1, std::memory_order_relaxed);
		g_lastAbsY.store(-1, std::memory_order_relaxed);
		return 0;
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

int main(int argc, char** argv) {
	std::srand((unsigned int)std::time(nullptr));

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
		else if (arg == "--help" || arg == "-h") {
			std::printf(
				"Usage:\n"
				"  cpp_receiver.exe --host <host> [--port 9090] --exePath <path> [--clientId <id>]\n"
				"Examples:\n"
				"  cpp_receiver.exe --host 127.0.0.1 --exePath \"C:\\\\Windows\\\\System32\\\\notepad.exe\"\n"
				"\n");
			return 0;
		}
	}

	if (clientId.empty()) clientId = RandomId(10);

	// Create full-screen window
	HINSTANCE hInstance = GetModuleHandle(nullptr);
	const wchar_t* className = L"cpp_receiver_window";
	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = className;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc);

	// First create window with max virtual screen size.
	const int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	const int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	g_hwnd = CreateWindowExW(
		0, className, L"cpp_receiver",
		WS_POPUP | WS_VISIBLE, sx, sy, sw, sh,
		nullptr, nullptr, hInstance, nullptr);
	if (!g_hwnd) {
		std::fprintf(stderr, "CreateWindowExW failed\n");
		return 1;
	}
	EnsureWindowSizeForFullscreen(g_hwnd);
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
		rtc::InitLogger(rtc::LogLevel::Info);
		EnsureD3D(g_hwnd);
		UpdateLetterboxRect();
	} catch (const std::exception& e) {
		std::fprintf(stderr, "Init D3D failed: %s\n", e.what());
		return 2;
	}

	// Run signaling/WebRTC in background thread.
	std::thread t([&] {
		try {
			RunSignalingAndWebRtc(host, port, clientId, exePath);
		} catch (const std::exception& e) {
			std::fprintf(stderr, "WebRTC thread error: %s\n", e.what());
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
	while (!g_exitRequested.load(std::memory_order_relaxed)) {
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		if (g_exitRequested.load(std::memory_order_relaxed)) break;

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

		// Publish texture update
		EnsureTextureIfNeeded(w, h);
		if (g_d3d.texture) {
			g_d3d.context->UpdateSubresource(
				g_d3d.texture.Get(),
				0, nullptr,
				localBgra.data(),
				w * 4,
				0);
		}
		UpdateLetterboxRectFor(w, h);

		// Draw
		RenderOneFrame(w, h);

		// Present and time it
		HRESULT hr = g_d3d.swapChain->Present(0, 0);
		(void)hr;
		LARGE_INTEGER qpcNow{};
		QueryPerformanceCounter(&qpcNow);
		const uint64_t presentQpc = (uint64_t)qpcNow.QuadPart;
		const uint64_t presentMs = QpcToSysMs(presentQpc);

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

