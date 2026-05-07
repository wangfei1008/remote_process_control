////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 转发路径目录（纯静态描述，不参与实际 Socket 投递）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 根据《接口.docx》记录 Signaling / DataChannel 上的典型上下游关系
// - 作为路由策略（Strategy）注入前的「目录」参考，本类无外部依赖
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef EXTERNAL_API_FORWARD_CATALOG_H
#define EXTERNAL_API_FORWARD_CATALOG_H

#include "external_api_protocol_constants.h"

#include <QString>

namespace external_api_forward_catalog {

/////////////////////////////////////////////////////////////////////////////
/// @说明 
///          会话启动请求在 Server 侧应映射为下发 Agent 的报文类型（文档 4.1）
/// @参数
///          无
/// @返回值 目标 type 字符串（agent_start_session）
///         
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
inline QString session_upstream_to_agent_downstream_type()
{
	return external_api_protocol_constants::k_type_agent_start_session();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明 
///          WebRTC SDP / ICE 在文档中为透传嵌套风格（不在此翻译载荷）
/// @参数
///          无
/// @返回值 是否为文档声明的透传族类型之一
///         
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
inline bool is_webrtc_pass_through_family(const QString &type_string)
{
	return type_string == external_api_protocol_constants::k_type_webrtc_offer();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明 
///          DataChannel 专属控制类报文（文档第 6、7 章）
/// @参数
///          type_string — JSON type 字段
/// @返回值 是否归属 DataChannel 典型范畴（启发式分类）
///         
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
inline bool is_typical_data_channel_family(const QString &type_string)
{
	return type_string == external_api_protocol_constants::k_type_file_list_req()
	       || type_string == external_api_protocol_constants::k_type_file_list_res()
	       || type_string == external_api_protocol_constants::k_type_file_download_init()
	       || type_string == external_api_protocol_constants::k_type_file_download_data()
	       || type_string == external_api_protocol_constants::k_type_file_upload_chunk()
	       || type_string == external_api_protocol_constants::k_type_file_upload_ack()
	       || type_string == external_api_protocol_constants::k_type_control_action()
	       || type_string == external_api_protocol_constants::k_type_latency_ping()
	       || type_string == external_api_protocol_constants::k_type_latency_pong();
}

} // namespace external_api_forward_catalog

#endif
