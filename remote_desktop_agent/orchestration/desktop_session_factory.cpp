#include "orchestration/desktop_session_factory.h"
#include "orchestration/active_desktop_session.h"

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          工厂方法：构造 active_desktop_session 并调用 wire_components 完成子系统装配
/// @参数
///          params--会话创建参数（信令、RTC、回调等）
/// @返回值
///          已装配的 active_desktop_session 共享指针
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
std::shared_ptr<active_desktop_session> desktop_session_factory::create_session(const desktop_session_create_params& params) const
{
    auto session = std::shared_ptr<active_desktop_session>(new active_desktop_session());
    session->wire_components(params);
    return session;
}
