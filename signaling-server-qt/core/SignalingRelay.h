#ifndef SIGNALINGRELAY_H
#define SIGNALINGRELAY_H

#include <QString>

class QWebSocket;
class ClientRegistry;

/**
 * 策略/路由：将 JSON 信令按 destination id 转发，并改写来源 id。
 * 单一职责：不包含套接字生命周期，仅做消息变换与投递。
 */
class SignalingRelay {
public:
	explicit SignalingRelay(ClientRegistry *registry);

	/**
	 * @param from 发送方连接
	 * @param messageText 原始 JSON 文本
	 * @return 是否已成功转发（目标存在且已发送）
	 */
	bool relay_text_message(QWebSocket *from, const QString &messageText) const;

private:
	ClientRegistry *registry_ = nullptr;
};

#endif
