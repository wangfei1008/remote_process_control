#include "h264_annexb_decoder.hpp"

#include "rdr_log.hpp"

#include <cstring>
#include <mutex>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct H264AnnexBDecoderToBGRA::Impl {
	explicit Impl(H264AnnexBDecoderToBGRA::PublishFrame publish)
		: publish_(std::move(publish)) {
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

	~Impl() {
		if (m_sws) sws_freeContext(m_sws);
		if (m_ctx) avcodec_free_context(&m_ctx);
		if (m_packet) av_packet_free(&m_packet);
		if (m_frame) av_frame_free(&m_frame);
	}

	bool decodeAnnexB(const uint8_t* data, size_t size) {
		std::lock_guard<std::mutex> lk(m_decodeMtx);

		if (!m_ctx || !publish_) return false;

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
			return false;
		}

		const int outW = m_frame->width;
		const int outH = m_frame->height;
		if (outW <= 0 || outH <= 0) return false;

		ensureSws(outW, outH, static_cast<AVPixelFormat>(m_frame->format));

		uint8_t* dstSlices[4] = { m_bgraTmp.data(), nullptr, nullptr, nullptr };
		int dstStride[4] = { outW * 4, 0, 0, 0 };
		sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, outH, dstSlices, dstStride);

		++decodedCount_;
		publish_(outW, outH, decodedCount_, std::move(m_bgraTmp));
		return true;
	}

private:
	void ensureSws(int outW, int outH, AVPixelFormat srcFmt) {
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

	H264AnnexBDecoderToBGRA::PublishFrame publish_;

	std::mutex m_decodeMtx;
	AVCodecContext* m_ctx = nullptr;
	AVPacket* m_packet = nullptr;
	AVFrame* m_frame = nullptr;
	SwsContext* m_sws = nullptr;
	int m_outW = 0;
	int m_outH = 0;
	AVPixelFormat m_srcFmt = AV_PIX_FMT_NONE;
	std::vector<uint8_t> m_bgraTmp;
	uint64_t decodedCount_ = 0;
};

H264AnnexBDecoderToBGRA::H264AnnexBDecoderToBGRA(PublishFrame publish)
	: impl_(std::make_unique<Impl>(std::move(publish))) {
}

H264AnnexBDecoderToBGRA::~H264AnnexBDecoderToBGRA() = default;

bool H264AnnexBDecoderToBGRA::decodeAnnexB(const uint8_t* data, size_t size) {
	return impl_->decodeAnnexB(data, size);
}
