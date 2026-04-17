#include "receiver_session.hpp"

#include "app_info.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
	ReceiverOptions opt;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--host" && i + 1 < argc) opt.host = argv[++i];
		else if (arg == "--port" && i + 1 < argc) opt.port = std::atoi(argv[++i]);
		else if (arg == "--clientId" && i + 1 < argc) opt.clientId = argv[++i];
		else if (arg == "--exePath" && i + 1 < argc) opt.exePath = argv[++i];
		else if (arg == "--decoder" && i + 1 < argc) {
			const std::string v = argv[++i];
			if (v == "hw" || v == "d3d11va") opt.decoderMode = RdrVideoDecoderMode::Hw;
			else if (v == "sw" || v == "software") opt.decoderMode = RdrVideoDecoderMode::Sw;
			else opt.decoderMode = RdrVideoDecoderMode::Auto;
		}
		else if (arg == "--windowed") opt.windowed = true;
		else if (arg == "--fullscreen") opt.windowed = false;
		else if (arg == "--allMonitors") opt.fullscreenAllMonitors = true;
		else if (arg == "--maxFps" && i + 1 < argc) opt.maxPresentFps = std::atoi(argv[++i]);
		else if (arg == "--help" || arg == "-h") {
			std::printf(
				"Usage:\n"
				"  %s --host <host> [--port 9090] --exePath <path> [--clientId <id>]\n"
				"Display / performance:\n"
				"  (default)           Windowed mode; use taskbar / other apps normally\n"
				"  --fullscreen        Borderless fullscreen (primary or --allMonitors)\n"
				"  --windowed          Same as default (explicit)\n"
				"  --allMonitors       With --fullscreen: cover all monitors\n"
				"  --maxFps <n>        Cap present rate (default 60; 0 = unlimited)\n"
				"  --decoder <mode>    Video decode: auto (default), hw (D3D11VA), sw (CPU)\n"
				"Examples:\n"
				"  %s --host 127.0.0.1 --exePath \"C:\\\\Windows\\\\System32\\\\notepad.exe\"\n"
				"\n",
				rdr::kExecutableName,
				rdr::kExecutableName);
			return 0;
		}
	}

	return ReceiverMain(opt);
}
