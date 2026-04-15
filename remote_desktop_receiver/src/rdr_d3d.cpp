#include "rdr_d3d.hpp"

#include "receiver_context.hpp"
#include "rdr_log.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

void RdrUpdateLetterboxRect() {
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
	const int w = (int)(std::max)(1.0, std::round(contentW));
	const int h = (int)(std::max)(1.0, std::round(contentH));

	g_lbX.store(x, std::memory_order_relaxed);
	g_lbY.store(y, std::memory_order_relaxed);
	g_lbW.store(w, std::memory_order_relaxed);
	g_lbH.store(h, std::memory_order_relaxed);
}

void RdrUpdateLetterboxRectFor(int vw, int vh) {
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
	const int w = (int)(std::max)(1.0, std::round(contentW));
	const int h = (int)(std::max)(1.0, std::round(contentH));

	g_lbX.store(x, std::memory_order_relaxed);
	g_lbY.store(y, std::memory_order_relaxed);
	g_lbW.store(w, std::memory_order_relaxed);
	g_lbH.store(h, std::memory_order_relaxed);
}

void RdrEnsureD3D(HWND hwnd) {
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

void RdrResizeD3D(HWND hwnd) {
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

	RdrUpdateLetterboxRect();
}

void RdrEnsureTextureIfNeeded(int w, int h) {
	if (w <= 0 || h <= 0) return;
	if (g_d3d.texture && g_d3d.textureSRV) {
		D3D11_TEXTURE2D_DESC td{};
		g_d3d.texture->GetDesc(&td);
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

void RdrRenderOneFrame(int vw, int vh) {
	(void)vw;
	(void)vh;

	float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
	ID3D11RenderTargetView* rtvs[] = { g_d3d.rtv.Get() };
	g_d3d.context->OMSetRenderTargets(1, rtvs, nullptr);
	g_d3d.context->ClearRenderTargetView(g_d3d.rtv.Get(), clearColor);

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

void RdrApplyFullscreenAllMonitors(HWND hwnd) {
	const int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	const int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
	SetWindowPos(hwnd, HWND_TOP, sx, sy, sw, sh, SWP_SHOWWINDOW);
}

void RdrApplyFullscreenPrimary(HWND hwnd) {
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

void RdrResizeWindowToVideoResolution(HWND hwnd, int videoW, int videoH) {
	if (!hwnd || !g_windowed || videoW <= 0 || videoH <= 0) return;

	static int s_lastAppliedVideoW = 0;
	static int s_lastAppliedVideoH = 0;
	if (s_lastAppliedVideoW == videoW && s_lastAppliedVideoH == videoH) return;

	RECT work{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const int workW = (std::max)(1, (int)(work.right - work.left));
	const int workH = (std::max)(1, (int)(work.bottom - work.top));

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
