#include "character_conversion.h"
#include <iostream>
#include <algorithm>
#include <cctype>

#ifndef _WIN32
#include <locale>
#include <codecvt>
#endif

std::string to_lower_ascii(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// UTF-8 → UTF-16 (std::wstring)
std::wstring utf8_to_wide(const std::string& utf8_str)
{
	if (utf8_str.empty()) return L"";

#ifdef _WIN32
	// 计算转换后的宽字符长度
	int wlen = MultiByteToWideChar(
		CP_UTF8,                                             // 输入编码：UTF-8
		0,                                                   // 标志位（默认）
		utf8_str.c_str(),                                    // 输入字符串
		static_cast<int>(utf8_str.length()),                 // 自动计算输入长度（-1 表示以 null 结尾）
		nullptr, 0                                           // 输出缓冲区置空以获取长度
	);
	if (wlen == 0) return L"";

	// 分配缓冲区并执行转换
	std::wstring wstr(wlen, 0);
	MultiByteToWideChar(CP_UTF8, 0,	utf8_str.c_str(), -1, &wstr[0], wlen);

	return wstr;
#else
	try {
		std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
		return converter.from_bytes(utf8_str);
	}
	catch (...) { return L""; }
#endif
}


// UTF-16 (std::wstring) → UTF-8 (std::string)
std::string wide_to_utf8(const std::wstring& wide_str)
{
	if (wide_str.empty()) return "";
#ifdef _WIN32
	// 计算转换后的 UTF-8 长度
	int ulen = WideCharToMultiByte(
		CP_UTF8,            // 目标编码：UTF-8
		0,                  // 标志位（默认）
		wide_str.c_str(),    // 输入宽字符串
		static_cast<int>(wide_str.length()),                 // 指定长度
		nullptr, 0,         // 输出缓冲区置空以获取长度
		nullptr, nullptr    // 默认字符处理
	);
	if (ulen == 0) return "";

	// 分配缓冲区并执行转换
	std::string utf8Str(ulen, 0);
	WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, &utf8Str[0], ulen, nullptr, nullptr );
	return utf8Str;
#else
	try {
		std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
		return converter.to_bytes(wide_str);
	}
	catch (...) { return ""; }
#endif
}

std::string utf8_to_ansi(const std::string& utf8_str)
{
	if (utf8_str.empty()) return "";

	// 先将 UTF-8 转为 UTF-16（wstring）
	std::wstring wide_str = utf8_to_wide(utf8_str);
	if (wide_str.empty()) return "";

	// 再将 UTF-16 转为 ANSI
	return wide_to_ansi(wide_str);
}

std::string ansi_to_utf8(const std::string& ansi_str)
{
	if (ansi_str.empty()) return "";

	// 先将 ANSI 转为 UTF-16（wstring）
	std::wstring wide_str = ansi_to_wide(ansi_str);
	if (wide_str.empty()) return "";

	// 再将 UTF-16 转为 UTF-8
	return wide_to_utf8(wide_str);
}

std::string wide_to_ansi(const std::wstring& wide_str) 
{
	if (wide_str.empty()) return "";
#ifdef _WIN32
	// 计算转换后的多字节字符串长度
	int ansi_length = WideCharToMultiByte(
		CP_ACP,             // 目标编码：系统本地编码（如 GBK）
		0,                  // 标志位（默认）
		wide_str.c_str(),    // 输入的宽字符串
		static_cast<int>(wide_str.length()),
		nullptr, 0,         // 输出缓冲区置空以获取长度
		nullptr, nullptr
	);
	if (ansi_length == 0) return "";

	// 分配缓冲区并执行转换
	std::string ansi_str(ansi_length, 0);
	WideCharToMultiByte( CP_ACP, 0, wide_str.c_str(), static_cast<int>(wide_str.length()), &ansi_str[0], ansi_length, nullptr, nullptr
	);

	if (!ansi_str.empty() && ansi_str.back() == '\0') 
		ansi_str.pop_back();

	return ansi_str;
#else
	size_t size = wcstombs(nullptr, wide_str.c_str(), 0);
	if (size == (size_t)-1) return "";
	std::string astr(size, 0);
	wcstombs(&astr[0], wide_str.c_str(), size + 1);
	return astr;
#endif
}

std::wstring ansi_to_wide(const std::string& ansi_str)
{
	if (ansi_str.empty()) return L"";
#ifdef _WIN32
	// 计算转换后所需缓冲区大小（字符数）
	int wideCharLen = MultiByteToWideChar(
		CP_ACP,             // ANSI代码页
		MB_PRECOMPOSED,     // 使用预组合字符
		ansi_str.c_str(),   // 源ANSI字符串
		-1,                 // 自动计算字符串长度（包含终止符）
		nullptr,            // 不接收转换结果（仅计算长度）
		0                   // 查询缓冲区大小
	);

	if (wideCharLen <= 0) return L"";

	// 分配宽字符缓冲区
	std::vector<wchar_t> buffer(wideCharLen);

	// 执行实际转换
	int result = MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, ansi_str.c_str(), -1, buffer.data(), wideCharLen );
	if (result <= 0) return L"";

	return std::wstring(buffer.data());
#else
	size_t size = mbstowcs(nullptr, ansi_str.c_str(), 0);
	if (size == (size_t)-1) return L"";
	std::wstring wstr(size, 0);
	mbstowcs(&wstr[0], ansi_str.c_str(), size + 1);
	return wstr;
#endif
}

/**
 * 将指定长度的 ANSI/MBCS 字符串转换为 UTF-8 字符串
 *
 * @param ansi_str  源 ANSI 字符串指针
 * @param size      源字符串长度（字节数）
 * @param code_page 源字符串的代码页（默认为系统 ANSI 代码页）
 *
 * @return 转换后的 UTF-8 字符串
 */
std::string ansi_to_utf8(LPCSTR ansi_str, int size, UINT code_page)
{
	// 处理空指针和空长度
	if (ansi_str == nullptr || size == 0)  return "";

	// 确保长度有效（非负）
	if (size < 0)  throw std::invalid_argument("Invalid size: size must be non-negative");
#ifdef _WIN32
	// 步骤1: 将 ANSI/MBCS 转换为宽字符 (UTF-16)
	int wstr_length = MultiByteToWideChar(
		code_page,          // 源字符串代码页
		MB_ERR_INVALID_CHARS, // 严格模式：遇到无效字符报错
		ansi_str,           // 源字符串
		size,               // 源字符串长度（字节数）
		nullptr,            // 不接收输出
		0                   // 输出缓冲区大小
	);

	// 检查转换结果
	if (wstr_length == 0) {
		DWORD error = GetLastError();
		throw std::runtime_error("ANSI to WideChar conversion failed. Error: " + std::to_string(error));
	}

	// 分配宽字符缓冲区
	std::vector<wchar_t> wstr_buffer(wstr_length + 1);  // +1 用于安全终止符

	int result = MultiByteToWideChar(
		code_page,
		0,
		ansi_str,
		size,
		wstr_buffer.data(),
		wstr_length
	);

	if (result == 0) {
		DWORD error = GetLastError();
		throw std::runtime_error("ANSI to WideChar conversion failed. Error: " + std::to_string(error));
	}

	// 显式添加终止符（API 不保证添加）
	wstr_buffer[wstr_length] = L'\0';

	// 步骤2: 将宽字符 (UTF-16) 转换为 UTF-8
	int utf8_length = WideCharToMultiByte(
		CP_UTF8,           // 目标编码：UTF-8
		WC_ERR_INVALID_CHARS, // 严格模式
		wstr_buffer.data(), // 源宽字符串
		wstr_length,       // 宽字符数（不含终止符）
		nullptr,           // 不接收输出
		0,                 // 输出缓冲区大小
		nullptr,           // 默认字符（不使用）
		nullptr            // 是否使用默认字符
	);

	if (utf8_length == 0) {
		DWORD error = GetLastError();
		throw std::runtime_error("WideChar to UTF-8 conversion failed. Error: " + std::to_string(error));
	}

	// 分配 UTF-8 缓冲区
	std::vector<char> utf8_buffer(utf8_length + 2);  // +1 用于安全终止符

	result = WideCharToMultiByte(
		CP_UTF8,
		0,
		wstr_buffer.data(),
		wstr_length,
		utf8_buffer.data(),
		utf8_length,
		nullptr,
		nullptr
	);

	if (result == 0) {
		DWORD error = GetLastError();
		throw std::runtime_error("WideChar to UTF-8 conversion failed. Error: " + std::to_string(error));
	}

	// 显式添加终止符
	utf8_buffer[utf8_length] = '\0';
	utf8_buffer[utf8_length + 1] = '\0';

	// 构造 std::string（包含可能的内嵌空字符）
	return std::string(utf8_buffer.data(), utf8_length);
#else
	// Linux 实现：将 ANSI (Locale) 转为 UTF-8
	// 注意：Linux 的 code_page 参数通常被忽略，除非手动调用 iconv
	std::string src(ansi_str, size);
	std::wstring wstr = ansi_to_wide(src);
	if (wstr.empty() && size > 0) throw std::runtime_error("ANSI to Wide conversion failed");
	return wide_to_utf8(wstr);
#endif
}


/**
 * 将包含 Unicode (UTF-16) 数据的缓冲区转换为 UTF-8 字符串
 *
 * @param data_ptr  指向包含 UTF-16 数据的缓冲区指针 (LPSTR 类型)
 * @param byte_size 缓冲区字节大小
 *
 * @return 转换后的 UTF-8 字符串
 */
std::string unicode_to_utf8(LPCSTR data_ptr, size_t byte_size) 
{
	// 处理空指针和空长度
	if (data_ptr == nullptr || byte_size == 0)  return "";

#ifdef _WIN32
	// 确保字节大小是偶数（UTF-16 要求 2 字节对齐）
	if (byte_size % sizeof(wchar_t) != 0) {
		throw std::invalid_argument("Buffer size must be multiple of wchar_t size (2 bytes)");
	}

	// 计算宽字符数量
	const size_t wchar_count = byte_size / sizeof(wchar_t);

	// 将 char* 重新解释为 wchar_t*
	const wchar_t* unicode_data = reinterpret_cast<const wchar_t*>(data_ptr);

	// 步骤1: 检查是否需要添加终止符
	bool add_null_terminator = false;
	if (wchar_count > 0 && unicode_data[wchar_count - 1] != L'\0') {
		add_null_terminator = true;
	}

	// 步骤2: 将 UTF-16 转换为 UTF-8
	int utf8_length = WideCharToMultiByte(
		CP_UTF8,                  // 目标编码：UTF-8
		WC_ERR_INVALID_CHARS,     // 严格模式：遇到无效字符报错
		unicode_data,             // 源宽字符串
		static_cast<int>(wchar_count) + (add_null_terminator ? 1 : 0), // 字符数
		nullptr,                  // 不接收输出
		0,                        // 输出缓冲区大小
		nullptr,                  // 默认字符（不使用）
		nullptr                   // 是否使用默认字符
	);

	if (utf8_length == 0) {
		DWORD error = GetLastError();
		throw std::runtime_error("UTF-16 to UTF-8 conversion failed. Error: " + std::to_string(error));
	}

	// 分配 UTF-8 缓冲区
	std::vector<char> utf8_buffer(utf8_length);

	int result = WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		unicode_data,
		static_cast<int>(wchar_count),
		utf8_buffer.data(),
		utf8_length,
		nullptr,
		nullptr
	);

	if (result == 0) {
		DWORD error = GetLastError();
		throw std::runtime_error("UTF-16 to UTF-8 conversion failed. Error: " + std::to_string(error));
	}

	// 如果源数据没有终止符，手动添加
	if (add_null_terminator) {
		// 检查转换结果是否已包含终止符
		if (utf8_length > 0 && utf8_buffer[utf8_length - 1] != '\0') {
			// 添加终止符
			utf8_buffer.push_back('\0');
		}
	}

	// 创建字符串（包含可能的内嵌空字符）
	return std::string(utf8_buffer.data(), utf8_length);
#else
	// Linux 实现：处理 2 字节的 UTF-16 数据
	if (byte_size % 2 != 0) throw std::invalid_argument("Buffer size must be even for UTF-16");

	const char16_t* u16_data = reinterpret_cast<const char16_t*>(data_ptr);
	size_t u16_len = byte_size / 2;

	try {
		std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
		return convert.to_bytes(u16_data, u16_data + u16_len);
	}
	catch (const std::exception& e) {
		throw std::runtime_error("UTF-16 to UTF-8 failed");
	}
#endif
}

std::string safe_unicode_to_utf8(LPCSTR data_ptr, size_t byte_size)
{
	if (byte_size < sizeof(wchar_t)) return ""; // 无法包含完整字符

#ifdef _WIN32
	// 检查最后一个字符是否完整
	const size_t wchar_count = byte_size / sizeof(wchar_t);
	const wchar_t* unicode_data = reinterpret_cast<const wchar_t*>(data_ptr);

	// 处理代理对（UTF-16 扩展字符）
	if (wchar_count >= 1) {
		wchar_t last_char = unicode_data[wchar_count - 1];

		// 如果是高代理项（需要低代理项）
		if (last_char >= 0xD800 && last_char <= 0xDBFF) {
			// 移除不完整的高代理项
			return unicode_to_utf8(data_ptr, byte_size - sizeof(wchar_t));
		}
	}

#else
	// Linux 下处理截断的 UTF-16 (char16_t)
	const uint16_t* u16_ptr = reinterpret_cast<const uint16_t*>(data_ptr);
	uint16_t last_unit = u16_ptr[(byte_size / 2) - 1];
	if (last_unit >= 0xD800 && last_unit <= 0xDBFF) {
		return unicode_to_utf8(data_ptr, byte_size - 2);
	}
#endif

	// 正常转换
	return unicode_to_utf8(data_ptr, byte_size);
}