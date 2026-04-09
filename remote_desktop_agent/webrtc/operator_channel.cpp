#include "webrtc/operator_channel.h"
#include "nlohmann/json.hpp"

namespace {

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          单人场景控制权限：DataChannel 打开时恒授予控制（无多客户端仲裁）
/// @参数
///          （见 ClientPeerConnection 回调签名）
/// @返回值
///          true
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
bool always_grant_control(const std::string&, std::shared_ptr<rtc::DataChannel>) { return true; }

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          释放控制占位：单人场景无操作
/// @参数
///          client_id--信令客户端 id
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void noop_release(const std::string&) {}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          恒认为当前客户端为控制者（与 always_grant_control 一致）
/// @参数
///          client_id--信令客户端 id
/// @返回值
///          true
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
bool always_is_controller(const std::string&) { return true; }

} // namespace

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          构造操作者通道：创建 PeerConnection、注册 ICE/状态回调、组装 ClientPeerConnection
/// @参数
///          config--libdatachannel RTC 配置
///          websocket--信令 WebSocket 弱引用
///          client_id--本连接在信令中的 id
///          file_service--本会话文件传输服务实例
///          enable_media_tracks--是否添加音视频轨
///          create_media_session--轨道就绪后拉取共享 remote_desktop_media_session 的工厂回调
///          send_offer--本地 SDP 就绪后发往信令的回调
///          io_dispatch--串行派发断开等事件的队列
///          on_connection_lost--PeerConnection 进入断开/失败/关闭时调用
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
operator_channel::operator_channel(rtc::Configuration config,
                                   std::weak_ptr<rtc::WebSocket> websocket,
                                   std::string client_id,
                                   std::shared_ptr<remote_file_transfer_controller> file_service,
                                   bool enable_media_tracks,
                                   create_media_session_callback create_media_session,
                                   send_signaling_fn send_offer,
                                   DispatchQueue& io_dispatch,
                                   connection_lost_fn on_connection_lost)
    : m_config(std::move(config))
    , m_websocket(std::move(websocket))
    , m_client_id(std::move(client_id))
    , m_send_signaling(std::move(send_offer))
    , m_io_dispatch(io_dispatch)
    , m_on_connection_lost(std::move(on_connection_lost))
{
    m_peer_connection = std::make_shared<rtc::PeerConnection>(m_config);

    m_peer_connection->onStateChange(
        [this](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed
                || state == rtc::PeerConnection::State::Closed) {
                m_io_dispatch.dispatch([fn = m_on_connection_lost]() {
                    if (fn) fn();
                });
            }
        });

    m_peer_connection->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state != rtc::PeerConnection::GatheringState::Complete) return;
        auto description = m_peer_connection->localDescription();
        if (!description || !m_send_signaling) return;
        nlohmann::json message = { { "id", m_client_id }, { "type", description->typeString() },
            { "sdp", std::string(description.value()) } };
        m_send_signaling(message.dump());
    });

    m_client_peer =
        std::make_shared<ClientPeerConnection>(m_peer_connection, m_websocket, m_client_id, always_grant_control,
            noop_release, always_is_controller, std::move(file_service), enable_media_tracks);
    m_client_peer->set_callback(std::move(create_media_session));
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          析构时关闭连接，释放轨与 PeerConnection
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
operator_channel::~operator_channel()
{
    close();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          触发生成本地 SDP（随后经 onGatheringStateChange 发出 Offer）
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void operator_channel::start_local_negotiation()
{
    if (m_peer_connection) m_peer_connection->setLocalDescription();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          设置远端 SDP Answer，完成握手后半段
/// @参数
///          sdp_text--Answer SDP 文本
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void operator_channel::set_remote_answer(const std::string& sdp_text)
{
    if (!m_peer_connection) return;
    auto description = rtc::Description(sdp_text, std::string("answer"));
    m_peer_connection->setRemoteDescription(description);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          关闭 PeerConnection 并重置内部 shared_ptr，忽略异常
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void operator_channel::close()
{
    try {
        if (m_peer_connection) m_peer_connection->close();
    } catch (...) {
    }
    m_client_peer.reset();
    m_peer_connection.reset();
}
