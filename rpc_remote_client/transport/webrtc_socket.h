#pragma once

#include <rtc/rtc.hpp>
#include <memory>
#include <string>
#include <cstdint>
#include "nlohmann/json.hpp"
#include "transport/dispatch_queue.hpp"
#include "transport/client_peer_connection.h"
#include "transport/file_transfer_service.h"
#include <unordered_map>
#include <mutex>

#define LOGINFO(...) 
#define LOGERROR(...)

class WebRTCSocket
{
public:
	WebRTCSocket();
    void start(const std::string& ip, int port);
private:
	void _init_signaling();
	void _on_message(nlohmann::json message);
	std::shared_ptr<rtc::PeerConnection> _init_peer_connection(const std::string& id);
	std::shared_ptr<Stream> _get_or_create_stream();
    void _stop_and_reset_stream();
    void _stop_stream_keep_clients();
    void _close_all_clients();
	bool _request_control(const std::string& clientId, std::shared_ptr<rtc::DataChannel>);
	void _release_control(const std::string& clientId);
	bool _is_controller(const std::string& clientId);
    void _broadcast_remote_process_exited();
private:
    std::shared_ptr<rtc::WebSocket> m_ws;
    std::string m_signaling_ip;
    std::string m_signaling_url;
	int m_signaling_port;
    std::string m_stun_server;
    std::string m_signaling_local_id;
    uint64_t m_frame_mark_interval = 10;
    uint64_t m_capture_health_interval = 30;
	DispatchQueue m_thread_queue; // WebRTC 操作使用单线程队列

	std::unordered_map<std::string, std::shared_ptr<ClientPeerConnection>> m_clients;
	rtc::Configuration m_config;

	// 共享的流与进程对象
	std::shared_ptr<Stream> m_stream;
	std::mutex m_stream_mtx;
	std::string m_exe_path;

	// 并发控制权限限制
	std::mutex m_control_mtx;
	std::string m_controller_id;
    std::shared_ptr<FileTransferService> m_file_transfer_service;
};

