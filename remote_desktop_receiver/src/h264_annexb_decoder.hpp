#pragma once

#include "h264_decoder_iface.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

/**
 * FFmpeg H.264 (Annex B) decode to packed BGRA8888.
 * Decoded frames are delivered via PublishFrame (typically into the UI/render thread shared buffer).
 */
class H264AnnexBDecoderToBGRA final : public IH264ToBGRADecoder {
public:
	using PublishFrame = std::function<void(int w, int h, uint64_t decodedIndex, std::vector<uint8_t>&& bgra)>;

	explicit H264AnnexBDecoderToBGRA(PublishFrame publish);
	~H264AnnexBDecoderToBGRA() override;

	H264AnnexBDecoderToBGRA(const H264AnnexBDecoderToBGRA&) = delete;
	H264AnnexBDecoderToBGRA& operator=(const H264AnnexBDecoderToBGRA&) = delete;

	bool decodeAnnexB(const uint8_t* data, size_t size) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
