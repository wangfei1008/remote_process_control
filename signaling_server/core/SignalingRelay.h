////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令 JSON 按 id 转发（将目标 id 改写为来源 id）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 从 JSON 根对象读取 id 作为目标 client_id
// - 不管理套接字生命周期
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef SIGNALINGRELAY_H
#define SIGNALINGRELAY_H

#include <QString>

class QWebSocket;
class ClientRegistry;

class SignalingRelay {
public:
	explicit SignalingRelay(ClientRegistry *client_registry);

	/////////////////////////////////////////////////////////////////////////////
	/// @说明
	///          将文本 JSON 转发至 registry[id]，并把 id 改写为来源连接名
	/// @参数
	///          from--发送方；message_text--原始 JSON 文本
	/// @返回值 是否已成功发送
	///
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	bool relay_text_message(QWebSocket *from, const QString &message_text) const;

private:
	ClientRegistry *m_client_registry = nullptr;
};

#endif
