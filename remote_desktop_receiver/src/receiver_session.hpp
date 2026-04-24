#pragma once

#include <string>

#include "rdr_signaling.hpp"

struct ReceiverOptions {
	std::string host = "192.168.3.15";
	int port = 9090;
	std::string clientId;
	std::string exePath = "C:\\Windows\\System32\\notepad.exe";
	RdrVideoDecoderMode decoderMode = RdrVideoDecoderMode::Auto;

	bool windowed = true;
	bool fullscreenAllMonitors = false;
	int maxPresentFps = 60;
};

/** Standalone WebRTC viewer / remote control client (signaling + decode + D3D present). */
int ReceiverMain(const ReceiverOptions& opt);
