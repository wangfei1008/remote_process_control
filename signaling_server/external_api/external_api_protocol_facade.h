////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 对外接口协议门面 — JSON 与 variant 信封互转（Facade + 工厂式解析）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 对上提供单一入口 parse_* / serialize_*，隐藏 QJson 细节
// - 解析路径按 type 字段分流到各载荷构造（工厂式分支），便于后续替换序列化策略
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef EXTERNAL_API_PROTOCOL_FACADE_H
#define EXTERNAL_API_PROTOCOL_FACADE_H

#include "external_api_envelope_types.h"

#include <QByteArray>
#include <QString>

#include <optional>

class external_api_protocol_facade {
public:
	external_api_protocol_facade() = default;

	/////////////////////////////////////////////////////////////////////////////
	/// @说明 
	///          将 UTF-8 JSON 文本解析为信封 variant（失败返回 std::nullopt）
	/// @参数
	///          json_text — QString 承载的 JSON
	/// @返回值 解析成功返回信封 variant，否则无值
	///         
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	std::optional<external_api_envelope_types::external_api_message_variant> parse_json_text(const QString &json_text) const;

	/////////////////////////////////////////////////////////////////////////////
	/// @说明 
	///          将 UTF-8 字节解析为信封 variant
	/// @参数
	///          utf8_json — UTF-8 编码 JSON
	/// @返回值 解析成功返回信封 variant，否则无值
	///         
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	std::optional<external_api_envelope_types::external_api_message_variant> parse_json_bytes(const QByteArray &utf8_json) const;

	/////////////////////////////////////////////////////////////////////////////
	/// @说明 
	///          将信封序列化为紧凑 JSON（UTF-8）
	/// @参数
	///          message — 已构造 variant
	/// @返回值 QByteArray UTF-8 JSON
	///         
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	QByteArray serialize_to_json_utf8(const external_api_envelope_types::external_api_message_variant &message) const;

	/////////////////////////////////////////////////////////////////////////////
	/// @说明 
	///          将信封序列化为 QString（UTF-16，内容为 JSON 文本）
	/// @参数
	///          message — 已构造 variant
	/// @返回值 QString JSON 文本
	///         
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	QString serialize_to_json_text(const external_api_envelope_types::external_api_message_variant &message) const;
};

#endif
