#pragma once

#include <cstddef>
#include <cstdint>

struct IH264ToBGRADecoder {
	virtual ~IH264ToBGRADecoder() = default;
	virtual bool decodeAnnexB(const uint8_t* data, size_t size) = 0;
};
