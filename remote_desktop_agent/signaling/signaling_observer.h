////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令事件观察者接口（Observer 模式）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 抽象「收到一条结构化信令事件后的处理」
//- signaling_transport 作为 Subject，通过 set_observer 注入具体观察者（如 session_director）
//- 解耦传输层与业务编排层
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "signaling/signaling_event.h"

class signaling_observer {
public:
    virtual ~signaling_observer() = default;
    virtual void on_signaling_event(const signaling_event& event) = 0;
};
