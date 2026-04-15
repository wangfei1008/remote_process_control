#include "h264_d3d11va_decoder.hpp"

#include "rdr_log.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <cstring>
#include <mutex>
#include <stdexcept>

#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

using Microsoft::WRL::ComPtr;

static AVPixelFormat GetHwFormat(AVCodecContext* /*ctx*/, const AVPixelFormat* pix_fmts) {
	for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == AV_PIX_FMT_D3D11) {
			return *p;
		}
	}
	return AV_PIX_FMT_NONE;
}

static void CreateDecodeDeviceOnAdapterOf(ID3D11Device* renderDevice,
	ComPtr<ID3D11Device>& outDev,
	ComPtr<ID3D11DeviceContext>& outCtx) {
	if (!renderDevice) {
		throw std::runtime_error("D3D11VA: null render device");
	}
	ComPtr<IDXGIDevice> dxgiDev;
	HRESULT hr = renderDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev));
	if (FAILED(hr)) {
		throw std::runtime_error("D3D11VA: QueryInterface IDXGIDevice failed");
	}
	ComPtr<IDXGIAdapter> adapter;
	hr = dxgiDev->GetAdapter(&adapter);
	if (FAILED(hr)) {
		throw std::runtime_error("D3D11VA: GetAdapter failed");
	}

	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
	D3D_FEATURE_LEVEL outLevel{};
	hr = D3D11CreateDevice(
		adapter.Get(),
		D3D_DRIVER_TYPE_UNKNOWN,
		nullptr,
		flags,
		levels,
		(UINT)(sizeof(levels) / sizeof(levels[0])),
		D3D11_SDK_VERSION,
		&outDev,
		&outLevel,
		&outCtx);
	if (FAILED(hr)) {
		throw std::runtime_error("D3D11VA: D3D11CreateDevice (decode) failed");
	}
}

struct H264D3D11VADecoderToBGRA::Impl {
	explicit Impl(ID3D11Device* renderDevice, H264D3D11VADecoderToBGRA::PublishFrame publish)
		: publish_(std::move(publish)) {
		CreateDecodeDeviceOnAdapterOf(renderDevice, m_decodeDev, m_decodeCtx);

		m_hwDevRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
		if (!m_hwDevRef) {
			throw std::runtime_error("FFmpeg D3D11VA: av_hwdevice_ctx_alloc failed");
		}
		auto* hwDev = reinterpret_cast<AVHWDeviceContext*>(m_hwDevRef->data);
		auto* d3d11va = static_cast<AVD3D11VADeviceContext*>(hwDev->hwctx);
		d3d11va->device = m_decodeDev.Get();
		d3d11va->device_context = m_decodeCtx.Get();
		if (av_hwdevice_ctx_init(m_hwDevRef) < 0) {
			av_buffer_unref(&m_hwDevRef);
			throw std::runtime_error("FFmpeg D3D11VA: av_hwdevice_ctx_init failed");
		}

		const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) {
			av_buffer_unref(&m_hwDevRef);
			throw std::runtime_error("FFmpeg D3D11VA: H264 decoder not found");
		}
		m_ctx = avcodec_alloc_context3(codec);
		if (!m_ctx) {
			av_buffer_unref(&m_hwDevRef);
			throw std::runtime_error("FFmpeg D3D11VA: alloc context failed");
		}
		m_ctx->hw_device_ctx = av_buffer_ref(m_hwDevRef);
		m_ctx->get_format = GetHwFormat;
		m_ctx->thread_count = 1;
		m_ctx->extra_hw_frames = 16;
		if (avcodec_open2(m_ctx, codec, nullptr) < 0) {
			avcodec_free_context(&m_ctx);
			av_buffer_unref(&m_hwDevRef);
			throw std::runtime_error("FFmpeg D3D11VA: avcodec_open2 failed");
		}

		m_packet = av_packet_alloc();
		m_frame = av_frame_alloc();
		m_swFrame = av_frame_alloc();
		if (!m_packet || !m_frame || !m_swFrame) {
			avcodec_free_context(&m_ctx);
			av_buffer_unref(&m_hwDevRef);
			throw std::runtime_error("FFmpeg D3D11VA: alloc packet/frame failed");
		}
	}

	~Impl() {
		if (m_sws) {
			sws_freeContext(m_sws);
		}
		if (m_ctx) {
			avcodec_free_context(&m_ctx);
		}
		if (m_packet) {
			av_packet_free(&m_packet);
		}
		if (m_frame) {
			av_frame_free(&m_frame);
		}
		if (m_swFrame) {
			av_frame_free(&m_swFrame);
		}
		if (m_hwDevRef) {
			av_buffer_unref(&m_hwDevRef);
		}
	}

	bool decodeAnnexB(const uint8_t* data, size_t size) {
		std::lock_guard<std::mutex> lk(m_decodeMtx);
		if (!m_ctx || !publish_) {
			return false;
		}

		av_packet_unref(m_packet);
		if (av_new_packet(m_packet, (int)size) < 0) {
			return false;
		}
		std::memcpy(m_packet->data, data, size);

		int ret = avcodec_send_packet(m_ctx, m_packet);
		if (ret < 0) {
			Logf("[decoder][d3d11va] avcodec_send_packet failed ret=%d\n", ret);
			return false;
		}

		ret = avcodec_receive_frame(m_ctx, m_frame);
		if (ret != 0) {
			return false;
		}

		const int outW = m_frame->width;
		const int outH = m_frame->height;
		if (outW <= 0 || outH <= 0) {
			return false;
		}

		AVFrame* srcForSws = m_frame;
		if (m_frame->format == AV_PIX_FMT_D3D11) {
			av_frame_unref(m_swFrame);
			ret = av_hwframe_transfer_data(m_swFrame, m_frame, 0);
			if (ret < 0) {
				Logf("[decoder][d3d11va] av_hwframe_transfer_data failed ret=%d\n", ret);
				return false;
			}
			srcForSws = m_swFrame;
		}

		const auto srcFmt = static_cast<AVPixelFormat>(srcForSws->format);
		ensureSws(outW, outH, srcFmt);

		uint8_t* dstSlices[4] = { m_bgraTmp.data(), nullptr, nullptr, nullptr };
		int dstStride[4] = { outW * 4, 0, 0, 0 };
		sws_scale(m_sws, srcForSws->data, srcForSws->linesize, 0, outH, dstSlices, dstStride);

		++decodedCount_;
		publish_(outW, outH, decodedCount_, std::move(m_bgraTmp));
		return true;
	}

private:
	void ensureSws(int outW, int outH, AVPixelFormat srcFmt) {
		if (m_sws && outW == m_outW && outH == m_outH && srcFmt == m_srcFmt) {
			const size_t need = (size_t)outW * (size_t)outH * 4;
			if (m_bgraTmp.size() != need) {
				m_bgraTmp.resize(need);
			}
			return;
		}

		if (m_sws) {
			sws_freeContext(m_sws);
		}
		m_sws = sws_getContext(outW, outH, srcFmt,
			outW, outH, AV_PIX_FMT_BGRA,
			SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!m_sws) {
			throw std::runtime_error("FFmpeg D3D11VA: sws_getContext failed");
		}

		m_outW = outW;
		m_outH = outH;
		m_srcFmt = srcFmt;
		m_bgraTmp.resize((size_t)outW * (size_t)outH * 4);
	}

	H264D3D11VADecoderToBGRA::PublishFrame publish_;

	std::mutex m_decodeMtx;

	ComPtr<ID3D11Device> m_decodeDev;
	ComPtr<ID3D11DeviceContext> m_decodeCtx;

	AVBufferRef* m_hwDevRef = nullptr;
	AVCodecContext* m_ctx = nullptr;
	AVPacket* m_packet = nullptr;
	AVFrame* m_frame = nullptr;
	AVFrame* m_swFrame = nullptr;
	SwsContext* m_sws = nullptr;
	int m_outW = 0;
	int m_outH = 0;
	AVPixelFormat m_srcFmt = AV_PIX_FMT_NONE;
	std::vector<uint8_t> m_bgraTmp;
	uint64_t decodedCount_ = 0;
};

H264D3D11VADecoderToBGRA::H264D3D11VADecoderToBGRA(ID3D11Device* renderDevice, PublishFrame publish)
	: impl_(std::make_unique<Impl>(renderDevice, std::move(publish))) {
}

H264D3D11VADecoderToBGRA::~H264D3D11VADecoderToBGRA() = default;

bool H264D3D11VADecoderToBGRA::decodeAnnexB(const uint8_t* data, size_t size) {
	return impl_->decodeAnnexB(data, size);
}
