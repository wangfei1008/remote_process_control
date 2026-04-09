////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 进程级运行时配置快照
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 在应用启动阶段从 runtime_config / 环境变量读取一次并缓存
//- 包含 STUN、信令本地 ID、文件根目录、分片大小、帧 Mark / 采集健康检查间隔等
//- 供信令、媒体管线、文件服务等模块只读使用，避免各处重复读配置
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <string>

struct runtime_settings {
    std::string stun_server;
    std::string signaling_local_id;
    std::string data_root;
    std::uint64_t file_chunk_size = 512 * 1024;
    std::uint64_t frame_mark_interval = 10;
    std::uint64_t capture_health_interval = 30;

    static runtime_settings load_from_environment();
};
