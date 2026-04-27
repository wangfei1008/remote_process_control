#pragma once

// ============================================================
// capture.h
// 统一入口头文件 —— 外部代码只需包含这一个头
//
// 典型用法：
//
//   #include "capture/capture.h"
//
//   // 1. 创建会话
//   capture::ProcessSession session({ "C:/App/app.exe", 0, true });
//   session.start();
//
//   // 2. 创建 resolver（依赖注入）
//   win32::Window  wops;
//   win32::Process prims;
//   capture::WindowScorePolicy score_policy;
//   capture::CaptureTargetResolver resolver({ wops, prims, score_policy });
//
//   // 3. 创建采集后端
//   auto backend = capture::create_capture_backend_from_config();
//
//   // 4. 创建管线
//   capture::FrameComposer composer;
//   capture::FrameFilter   filter;
//   capture::CapturePipeline pipeline({ *backend, composer, filter });
//
//   // 5. 主循环
//   HWND main_hwnd = nullptr;
//   while (session.is_launch_running()) {
//       // 解析采集目标
//       capture::CaptureTargetResult target = resolver.resolve({
//           session.capture_pid(),
//           session.launch_pid(),
//           session.target_basename_lower(),
//           main_hwnd,
//           /*allow_pid_rebind=*/true
//       });
//       // 接受 rebind 建议
//       if (target.diag.pid_rebound)
//           session.rebind_capture_pid(target.capture_pid);
//       main_hwnd = target.main_hwnd;
//
//       // 采集一帧
//       rpc_video_contract::RawFrame frame;
//       rpc_video_contract::TelemetrySnapshot telem;
//       pipeline.grab_raw_frame(target.surfaces, now_ms, prep_ms, frame_id,
//                                /*filter_black=*/true, frame, telem);
//   }
// ============================================================

// ---- 基础设施 -----------------------------------------------
#include "infra/win32_types.h"
#include "infra/win32_process.h"
#include "infra/win32_window.h"

// ---- 会话 ---------------------------------------------------
#include "session/process_session.h"

// ---- 策略 ---------------------------------------------------
#include "policy/window_score_policy.h"
#include "policy/capture_target_resolver.h"

// ---- 后端 ---------------------------------------------------
#include "backend/capture_backend_kind.h"
#include "backend/window_tile.h"
#include "backend/i_capture_backend.h"
#include "backend/dxgi_capture_backend.h"
#include "backend/gdi_capture_backend.h"
#include "backend/capture_backend_factory.h"

// ---- 管线 ---------------------------------------------------
#include "pipeline/frame_filter.h"
#include "pipeline/frame_composer.h"
#include "pipeline/capture_pipeline.h"
