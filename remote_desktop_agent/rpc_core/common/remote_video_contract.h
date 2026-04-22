#pragma once

#include <cstddef>
#include <cstdint>

// 契约层（跨模块稳定）：只承载“语义”，不承载线程/队列/策略等实现细节。
// 设计目标：
// - 显式：bitstream 格式、时间戳体系（媒体时间 vs 墙钟）、颜色语义、buffer 所有权
// - 可扩展：struct_size + reserved + ext（向后兼容）
// - 跨平台：不依赖平台头文件；GPU 句柄以抽象 handle 表达


namespace rpc_video_contract {

// ---- 时间与尺寸 ----

// 媒体时间：默认 microseconds（用于 pipeline 节奏/同步）
using TimeUs = int64_t;

struct VideoSize {
    int32_t w = 0;
    int32_t h = 0;
};

struct VideoRect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
};

enum class Rotation : uint8_t { R0 = 0, R90 = 1, R180 = 2, R270 = 3 };

// ---- 像素/颜色（RawFrame）----

enum class PixelFormat : uint16_t {
    Unknown = 0,

    // 当前工程实际用到：capture RGB24 / receiver BGRA
    RGB24 = 1,
    BGRA8888 = 2,

    // 预留：常见 YUV（为后续零拷贝/GPU 直通/硬编做准备）
    NV12 = 10,
    I420 = 11,
    P010 = 12,
};

enum class ColorRange : uint8_t { Unknown = 0, Limited = 1, Full = 2 };
enum class ColorPrimaries : uint8_t { Unknown = 0, BT709 = 1, BT601 = 2, BT2020 = 3, DisplayP3 = 4 };
enum class TransferCharacteristics : uint8_t { Unknown = 0, BT709 = 1, SRGB = 2, PQ = 3, HLG = 4 };
enum class MatrixCoefficients : uint8_t { Unknown = 0, BT709 = 1, BT601 = 2, BT2020_NCL = 3 };

struct ColorDescription {
    ColorPrimaries primaries = ColorPrimaries::Unknown;
    TransferCharacteristics transfer = TransferCharacteristics::Unknown;
    MatrixCoefficients matrix = MatrixCoefficients::Unknown;
    ColorRange range = ColorRange::Unknown;
};

// ---- Buffer 所有权（可选零拷贝）----

struct ByteSpan {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

struct MutableByteSpan {
    uint8_t* data = nullptr;
    uint32_t size = 0;
};

using ReleaseFn = void (*)(void* opaque);

struct OwnedBuffer {
    // 如果需要把 buffer 的生命周期移交给接收方，设置 release；否则 release=nullptr 表示“不允许/不需要释放”
    MutableByteSpan bytes{};
    void* opaque = nullptr;
    ReleaseFn release = nullptr;
};

// GPU buffer 句柄抽象（契约层只描述，不绑定具体平台类型）
enum class GpuApi : uint8_t { Unknown = 0, Metal = 1, D3D11 = 2, Vulkan = 3, Cuda = 4, DmaBuf = 5 };

struct GpuBufferRef {
    GpuApi api = GpuApi::Unknown;
    uint64_t handle = 0; // 由 api 解释：指针/整数句柄/FD 等
    void* opaque = nullptr;
    ReleaseFn release = nullptr;
};

// ---- RawFrame（未压缩帧）----

enum class FrameStorageKind : uint8_t { Unknown = 0, Cpu = 1, Gpu = 2 };

struct VideoPlane {
    uint8_t* data = nullptr;
    int32_t stride_bytes = 0;
    uint32_t size_bytes = 0;
};

struct RawFrame {
    // 内容帧 id：建议贯穿 capture->encode->send（如果 sender 另有发送序号，可单独放 ext）
    uint64_t frame_id = 0;

    // 媒体时间
    TimeUs pts_us = 0;
    TimeUs dts_us = 0;

    // 分辨率语义
    VideoSize coded_size{};
    VideoRect visible_rect{};
    VideoSize display_size{}; // 0 表示“同 visible”

    PixelFormat format = PixelFormat::Unknown;
    Rotation rotation = Rotation::R0;
    ColorDescription color{};

    // 屏幕内容提示（UI/文本）
    bool is_screen_content = false;

    FrameStorageKind storage = FrameStorageKind::Unknown;

    // CPU：plane 描述（与 format 对应）
    static constexpr int kMaxPlanes = 4;
    VideoPlane planes[kMaxPlanes]{};
    uint8_t plane_count = 0;
    OwnedBuffer owned{}; // 可选：承载 planes 的底层内存（或用 ext 承载外部 buffer 引用）

    // GPU：例如 D3D11 texture / Metal texture
    GpuBufferRef gpu{};

    // 扩展
    uint32_t struct_size = sizeof(RawFrame);
    uint32_t reserved_u32[8] = {};
    void* ext = nullptr;
};

// ---- EncodedFrame（压缩帧 / Access Unit）----

enum class VideoCodec : uint8_t { Unknown = 0, H264 = 1, H265 = 2, VP9 = 3, AV1 = 4 };

enum class BitstreamFormat : uint8_t { Unknown = 0, AnnexB = 1, Avcc = 2, Hvcc = 3 };

enum class EncodedFrameType : uint8_t { Unknown = 0, IDR = 1, I = 2, P = 3, B = 4 };

// 端到端延迟链路字段（与当前工程使用的 H264 SEI 一一对应）
struct LatencyMarks {
    // 发送序号（当前工程：sender 侧 m_video_frame_index 写入 SEI frameId）
    uint64_t send_frame_id = 0;

    TimeUs prep_ms = 0; // grab 前
    TimeUs cap_ms = 0;  // capture done
    TimeUs enc_ms = 0;  // encode done
    TimeUs send_ms = 0; // sender send time

    bool has_prep_ms = false;
    bool valid = false;
};

struct EncodedFrame {
    // 内容帧 id（建议与 RawFrame.frame_id 同源）
    uint64_t frame_id = 0;

    // 媒体时间
    TimeUs pts_us = 0;
    TimeUs dts_us = 0;

    // 编码语义
    VideoCodec codec = VideoCodec::Unknown;
    BitstreamFormat bitstream = BitstreamFormat::Unknown;
    EncodedFrameType frame_type = EncodedFrameType::Unknown;
    VideoSize coded_size{};

    // payload：两种持有方式
    ByteSpan payload{};
    OwnedBuffer owned_payload{}; // 若 payload.data 指向 owned_payload.bytes.data，可用 release 归还

    // extradata：SPS/PPS/VPS/AV1 seq hdr 等（可 out-of-band）
    ByteSpan codec_extra{};

    // 端到端延迟 marks（可选：发送端注入/接收端解析后回填）
    LatencyMarks latency{};

    uint32_t struct_size = sizeof(EncodedFrame);
    uint32_t reserved_u32[8] = {};
    void* ext = nullptr;
};

// ---- TelemetrySnapshot（遥测快照）----

enum class CaptureBackend : uint8_t { Unknown = 0, Gdi = 1, Dxgi = 2 };

struct TelemetrySnapshot {
    VideoSize capture_size{};
    CaptureBackend backend = CaptureBackend::Unknown;

    // 墙钟链路（用于 sender SEI/DataChannel）
    TimeUs last_frame_unix_ms = 0;
    TimeUs last_prep_unix_ms = 0;
    TimeUs last_capture_unix_ms = 0;
    TimeUs last_encode_unix_ms = 0;

    uint32_t struct_size = sizeof(TelemetrySnapshot);
    uint32_t reserved_u32[8] = {};
    void* ext = nullptr;
};

// ---- ControlEvents（控制/状态事件：通常走 DataChannel JSON）----

struct VideoResolutionEvent {
    VideoSize size{};
};

struct CaptureHealthEvent {
    CaptureBackend backend = CaptureBackend::Unknown;
};

} // namespace rpc_video_contract

