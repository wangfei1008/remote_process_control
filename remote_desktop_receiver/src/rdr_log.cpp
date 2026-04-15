#include "rdr_log.hpp"

#include "app_info.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;
static std::atomic<bool> g_logReady{false};

void Logf(const char* fmt, ...) {
	if (!g_logReady.load(std::memory_order_relaxed)) return;
	char buf[2048];
	va_list args;
	va_start(args, fmt);
#if defined(_MSC_VER)
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
#else
	std::vsnprintf(buf, sizeof(buf), fmt, args);
#endif
	va_end(args);
	if (buf[0] == '\0') return;
	EnterCriticalSection(&g_logCs);
	DWORD written = 0;
	(void)WriteFile(g_logFile, buf, (DWORD)std::strlen(buf), &written, nullptr);
	LeaveCriticalSection(&g_logCs);
}

LONG WINAPI UnhandledExceptionLogger(EXCEPTION_POINTERS* ep) {
	DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
	void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
	Logf("[receiver] unhandled exception code=0x%08lX addr=%p\n", (unsigned long)code, addr);
	return EXCEPTION_EXECUTE_HANDLER;
}

void InitLogFile(const std::string& clientId) {
	const std::string logPath = rdr::DefaultLogFilename(clientId, (unsigned long long)GetCurrentProcessId());

	g_logFile = CreateFileA(
		logPath.c_str(),
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (g_logFile == INVALID_HANDLE_VALUE) return;

	InitializeCriticalSection(&g_logCs);
	g_logReady.store(true, std::memory_order_release);

	Logf("[log] started clientId=%s pid=%llu\n", clientId.c_str(), (unsigned long long)GetCurrentProcessId());
}
