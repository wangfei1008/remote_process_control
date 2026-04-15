#pragma once

#include "rdr_win32_include.hpp"

#include <string>

void Logf(const char* fmt, ...);
void InitLogFile(const std::string& clientId);
LONG WINAPI UnhandledExceptionLogger(EXCEPTION_POINTERS* ep);
