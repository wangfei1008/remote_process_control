#pragma once

#include "h264_decoder_iface.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct ID3D11Device;

/**
 * FFmpeg H.264 (Annex B) via D3D11VA, then transfer to CPU and convert to packed BGRA8888.
 * Uses a dedicated D3D11 device (same adapter as the passed-in render device) so the
 * immediate context is not shared with the UI/render thread.
 */
class H264D3D11VADecoderToBGRA final : public IH264ToBGRADecoder {
public:
	using PublishFrame = std::function<void(int w, int h, uint64_t decodedIndex, std::vector<uint8_t>&& bgra)>;

	explicit H264D3D11VADecoderToBGRA(ID3D11Device* renderDevice, PublishFrame publish);
	~H264D3D11VADecoderToBGRA() override;

	H264D3D11VADecoderToBGRA(const H264D3D11VADecoderToBGRA&) = delete;
	H264D3D11VADecoderToBGRA& operator=(const H264D3D11VADecoderToBGRA&) = delete;

	bool decodeAnnexB(const uint8_t* data, size_t size) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
