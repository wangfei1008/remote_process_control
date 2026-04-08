#include "bootstrap/runtime_settings.h"
#include "app/runtime_config.h"
#include <algorithm>

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          从 runtime_config / 环境变量读取 STUN、信令 ID、文件目录及媒体相关数值，生成只读配置快照
/// @参数
///          无
/// @返回值
///          填充后的 runtime_settings 对象
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
runtime_settings runtime_settings::load_from_environment()
{
    runtime_settings s;
    s.stun_server = runtime_config::get_string("RPC_WEBRTC_STUN_SERVER", "stun.l.google.com:19302");
    s.signaling_local_id = runtime_config::get_string("RPC_SIGNALING_LOCAL_ID", "server");
    s.data_root = runtime_config::get_string("RPC_DATA_ROOT", "D:\\rpc_data");
    s.file_chunk_size = static_cast<std::uint64_t>(
        (std::max)(1, runtime_config::get_int("RPC_FILE_CHUNK_SIZE", static_cast<int>(512 * 1024))));
    s.frame_mark_interval =
        static_cast<std::uint64_t>((std::max)(1, runtime_config::get_int("RPC_FRAME_MARK_INTERVAL", 10)));
    s.capture_health_interval =
        static_cast<std::uint64_t>((std::max)(1, runtime_config::get_int("RPC_CAPTURE_HEALTH_INTERVAL", 30)));
    return s;
}
