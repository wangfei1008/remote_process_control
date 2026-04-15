#pragma once

#include <string>

namespace rdr {

/** Win32 window class name (must be stable across runs for RegisterClass). */
inline constexpr wchar_t kWindowClassName[] = L"RemoteDesktopReceiverWnd";

/** Default window caption before stream metadata arrives. */
inline constexpr wchar_t kDefaultWindowTitle[] = L"Remote Desktop Receiver";

/** Console / help text: expected output binary name after build. */
inline constexpr char kExecutableName[] = "remote_desktop_receiver.exe";

/** Log file prefix: `<stem>_<clientId>_<pid>.log` */
inline std::string DefaultLogFilename(const std::string& clientId, unsigned long long pid) {
	return std::string("remote_desktop_receiver_") + clientId + "_" + std::to_string(pid) + ".log";
}

} // namespace rdr
