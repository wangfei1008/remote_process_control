#pragma once
#include <string>
#include <vector>

// Define Windows-style types for Linux compatibility if not on Windows
#ifndef _WIN32
	typedef unsigned int UINT;
	typedef const char* LPCSTR;
	typedef char* LPSTR;
	#define CP_ACP 0
	#define CP_UTF8 65001
	#define GetLastError() errno
#else
	#include <windows.h>
#endif


// UTF-8 → UTF-16 (std::wstring)
std::wstring utf8_to_wide(const std::string& utf8_str);

// UTF-16 (std::wstring) → UTF-8 (std::string)
std::string wide_to_utf8(const std::wstring& wide_str);

// UTF-8 → ANSI/MBCS (std::string)
std::string utf8_to_ansi(const std::string& utf8_str);

// ANSI/MBCS (std::string) → UTF-8 (std::string)
std::string ansi_to_utf8(const std::string& ansi_str);

// 将宽字符字符串转换为 ANSI/MBCS 字符串
std::string wide_to_ansi(const std::wstring& wide_str);

// 将 ANSI/MBCS 字符串转换为宽字符字符串
std::wstring ansi_to_wide(const std::string& ansi_str);


/**
 * 将指定长度的 ANSI/MBCS 字符串转换为 UTF-8 字符串
 *
 * @param ansi_str  源 ANSI 字符串指针
 * @param size      源字符串长度（字节数）
 * @param code_page 源字符串的代码页（默认为系统 ANSI 代码页）
 *
 * @return 转换后的 UTF-8 字符串
 */
std::string ansi_to_utf8(LPCSTR ansi_str, int size, UINT code_page = CP_ACP);

/**
 * 将包含 Unicode (UTF-16) 数据的缓冲区转换为 UTF-8 字符串
 *
 * @param data_ptr  指向包含 UTF-16 数据的缓冲区指针 (LPSTR 类型)
 * @param byte_size 缓冲区字节大小
 *
 * @return 转换后的 UTF-8 字符串
 */
std::string unicode_to_utf8(LPCSTR data_ptr, size_t byte_size);
std::string safe_unicode_to_utf8(LPCSTR data_ptr, size_t byte_size);

// ASCII 小写（与区域设置无关），用于 exe/window/class 等规范化比较。
std::string to_lower_ascii(std::string s);