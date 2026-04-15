#pragma once

#include <string>

enum class RdrVideoDecoderMode {
	Auto,
	Hw,
	Sw,
};

void RdrEnsureWinsockOnce();
void RdrRunSignalingAndWebRtc(
	const std::string& host,
	int port,
	const std::string& clientId,
	const std::string& exePath,
	RdrVideoDecoderMode decoderMode = RdrVideoDecoderMode::Auto);
