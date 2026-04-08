////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 会话替换策略抽象（Strategy 模式）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 定义「新媒体类信令到达时是否应 teardown 当前会话」的策略接口
//- always_replace_session_policy：固定为必须替换，符合产品公理（单一媒体会话 + 新请求替换）
//- 保留抽象便于未来扩展（例如并发互斥、拒绝新请求等策略）
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

class session_replace_policy {
public:
    virtual ~session_replace_policy() = default;
    virtual bool should_replace_existing_session_on_new_media_request() const = 0;
};

class always_replace_session_policy final : public session_replace_policy {
public:
    bool should_replace_existing_session_on_new_media_request() const override { return true; }
};
