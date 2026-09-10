#include "app/pipeline_instance.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <utility>

// 【2026-09-10 检测结果推送】Unix domain socket（判罚进程阻塞收，替代 HTTP 轮询）
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ai/ax_resize_map.hpp"
#include "codec/ax_jpeg_codec.h"
#include "common/ax_drawer.h"
#include "common/ax_image_processor.h"

namespace axpipeline::app {

namespace {

bool EnvFlagEnabled(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return false;
    const std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "YES";
}

std::uint32_t AlignEven(std::uint32_t v) noexcept {
    return (v % 2U) ? (v - 1U) : v;
}

// 【2026-09-10 检测结果推送】【2026-09-11 修复】按「整条消息」原子写（非阻塞 Unix socket）。
// 原实现是 header 写一次、每个 ball 再各写一次：fd 是非阻塞的，中途 EAGAIN 会在对端
// 留下「半条消息」，而客户端既不校验 magic、ball_count 也无上限 → 一旦错位就永久错位
// （判罚结果全乱，且不会自愈）。现在改成先把整条消息拼进连续 buffer 再写：
//   返回值  1 = 整条写完；
//           0 = 一个字节都没写出去（buffer 满）→ 干净丢帧，连接保持；
//          -1 = 写了半条，或连接已断（EPIPE/ECONNRESET/EBADF）→ 调用方必须断连，
//               让客户端重连后从消息边界重新对齐。
int WriteMessageAtomic(int fd, const void* buf, std::size_t n) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(fd, p + off, n - off);
        if (w > 0) {
            off += static_cast<std::size_t>(w);
            continue;
        }
        if (w < 0 && errno == EINTR) continue;                       // 被信号打断，重试
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // buffer 满
        return -1;                                                    // 连接不可用
    }
    if (off == n) return 1;
    return (off == 0) ? 0 : -1;  // 一个字节没写=干净丢帧；写了半条=链路已错位
}

// 【2026-09-11 修复】探测对端是否已关闭（MSG_PEEK 不消费数据）。
// 用于 accept 时判断「旧连接是死是活」，避免新判罚进程被旧连接永久挡住。
bool PeerClosed(int fd) noexcept {
    char probe = 0;
    const ssize_t r = ::recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) return true;                                                    // 对端已关闭
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;        // 活着且暂无数据
    return false;                                                               // 有数据 / 其它错误：按活着处理
}

std::uint64_t NowMs() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// 【2026-09-11】连接接管保护窗：刚接管过再次收到新连接时，短暂忽略，
// 避免两个都还活着的判罚进程每 0.5s 互踢一次（各拿一半帧比饿死更糟）。
constexpr std::uint64_t kDetHandoverGuardMs = 2000;

}  // namespace

PipelineInstance::PipelineInstance(ConfigLoader::PipelineCfg cfg)
    : cfg_(std::move(cfg)),
      npu_ok_(std::make_shared<std::atomic<std::uint64_t>>(0)),
      npu_err_(std::make_shared<std::atomic<std::uint64_t>>(0)),
      frame_counter_(std::make_shared<std::atomic<std::uint64_t>>(0)),
      frame_info_(std::make_shared<FrameInfo>()) {}

PipelineInstance::~PipelineInstance() {
    Stop();
    Close();
}

std::string PipelineInstance::name() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_.name;
}

ConfigLoader::PipelineCfg PipelineInstance::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
}

bool PipelineInstance::BuildPipeline(std::string* error) {
    auto p = axvsdk::pipeline::CreatePipeline();
    if (!p) {
        if (error) *error = "CreatePipeline failed";
        return false;
    }
    if (!p->Open(cfg_.sdk)) {
        if (error) *error = "pipeline Open failed";
        return false;
    }

    pipe_ = std::move(p);

    // Best-effort: populate source geometry early.
    {
        const auto stream = pipe_->GetInputStreamInfo();
        if (stream.width != 0 && stream.height != 0) {
            frame_info_->source_w.store(stream.width, std::memory_order_relaxed);
            frame_info_->source_h.store(stream.height, std::memory_order_relaxed);
        }
    }

    BuildFrameCallback();
    return true;
}

void PipelineInstance::BuildFrameCallback() {
    auto* pipe_ptr = pipe_.get();
    const auto p = cfg_;
    const auto counter = frame_counter_;
    const auto npu_worker = npu_worker_;
    const auto fi = frame_info_;

    pipe_ptr->SetFrameCallback([p, counter, npu_worker, fi](axvsdk::common::AxImage::Ptr frame) {
        if (!frame) return;
        fi->infer_w.store(frame->width(), std::memory_order_relaxed);
        fi->infer_h.store(frame->height(), std::memory_order_relaxed);
        if (fi->source_w.load(std::memory_order_relaxed) == 0 &&
            p.sdk.frame_output.output_image.width == 0 &&
            p.sdk.frame_output.output_image.height == 0) {
            fi->source_w.store(frame->width(), std::memory_order_relaxed);
            fi->source_h.store(frame->height(), std::memory_order_relaxed);
        }

        const auto n = counter->fetch_add(1, std::memory_order_relaxed) + 1;

        // 计时：frame callback（VDEC 解码输出）节奏
        {
            static std::chrono::steady_clock::time_point last{};
            const auto now = std::chrono::steady_clock::now();
            if ((n % 30) == 0) {
                if (last.time_since_epoch().count() != 0) {
                    const auto d = std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
                    std::fprintf(stderr, "[pipeline_frame] frame=%llu delta_us=%lld\n",
                                 (unsigned long long)n, (long long)d);
                }
                last = now;
            }
        }

        const auto every = p.log_every_n_frames == 0 ? 30U : p.log_every_n_frames;
        if ((n % every) == 0) {
            std::cout << "[pipeline=" << p.name << " dev=" << p.device_id << "] "
                      << "frame=" << n
                      << " fmt=" << static_cast<int>(frame->format())
                      << " w=" << frame->width()
                      << " h=" << frame->height()
                      << " stride0=" << frame->stride(0)
                      << " stride1=" << frame->stride(1)
                      << " mem=" << static_cast<int>(frame->memory_type())
                      << " phy0=0x" << std::hex << frame->physical_address(0) << std::dec
                      << " phy1=0x" << std::hex << frame->physical_address(1) << std::dec;
            if (EnvFlagEnabled("AXP_NPU_INFO")) {
                std::cout << " vir0=0x" << std::hex
                          << reinterpret_cast<std::uintptr_t>(frame->virtual_address(0))
                          << " vir1=0x"
                          << reinterpret_cast<std::uintptr_t>(frame->virtual_address(1))
                          << std::dec;
            }
            std::cout << "\n";
        }

        if (npu_worker) {
            npu_worker->Submit(std::move(frame), n);
        }
    });
}

void PipelineInstance::StoreLastDetections(std::vector<ai::Detection> dets, std::uint64_t seq, std::uint64_t pts_ms) noexcept {
    std::lock_guard<std::mutex> lock(det_mutex_);
    last_det_seq_ = seq;
    last_dets_ = dets;  // 最新帧（源图坐标，preview 画框用）
    det_queue_.push_back(DetectionBatch{seq, pts_ms, std::move(dets)});
    while (det_queue_.size() > kDetQueueCapacity) {
        det_queue_.pop_front();
    }
}

// 【2026-09-11 修复】原名为 GetLastDetectionsLocked（名字暗示「调用方需持锁」），
// 但实现里自己加了 det_mutex_。当前唯一调用点只持 mu_ 所以没出事，
// 但任何持 det_mutex_ 的调用方一旦调用它就会自死锁。改名 + 注释明确锁语义。
std::vector<ai::Detection> PipelineInstance::GetLastDetections() const {
    std::lock_guard<std::mutex> lock(det_mutex_);  // 本函数自己加锁，调用方不得持 det_mutex_
    return last_dets_;
}

std::vector<DetectionBatch> PipelineInstance::DrainDetections(std::uint64_t since_seq) {
    std::lock_guard<std::mutex> lock(det_mutex_);
    std::vector<DetectionBatch> out;
    out.reserve(det_queue_.size());
    for (const auto& b : det_queue_) {
        if (b.seq > since_seq) {
            out.push_back(b);
        }
    }
    return out;
}

void PipelineInstance::StartDetectionsSocket(const std::string& name) {
    StopDetectionsSocket();
    det_sock_path_ = "/tmp/ax_det_" + name + ".sock";
    ::unlink(det_sock_path_.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, det_sock_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return;
    }
    // 【2026-09-11 修复】backlog 由 1 提到 8：backlog=1 时若有残留连接占位，
    // 新判罚进程 connect 可能直接失败（ECONNREFUSED/阻塞），排查时无任何日志可看。
    if (::listen(fd, 8) != 0) {
        ::close(fd);
        return;
    }
    // 非阻塞 accept（on_result 里顺带 accept，不阻塞检测线程）
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    det_listen_fd_ = fd;
}

void PipelineInstance::StopDetectionsSocket() noexcept {
    if (det_conn_fd_ >= 0) { ::close(det_conn_fd_); det_conn_fd_ = -1; }
    if (det_listen_fd_ >= 0) { ::close(det_listen_fd_); det_listen_fd_ = -1; }
    det_conn_since_ms_ = 0;
    if (!det_sock_path_.empty()) { ::unlink(det_sock_path_.c_str()); det_sock_path_.clear(); }
}

void PipelineInstance::PushDetections(const std::vector<ai::Detection>& dets,
                                      std::uint64_t seq, std::uint64_t pts_ms,
                                      const char* stream_name) noexcept {
    // 1) accept：判罚进程 connect 时建立连接（listen fd 非阻塞，不阻塞检测线程）。
    //    【2026-09-11 修复】原来只在 det_conn_fd_ < 0 时 accept 一次，于是：
    //    「旧的、还活着的判罚进程」占住唯一连接后，新起的判罚进程会卡在 backlog 里
    //    永远不被 accept → 静默饿死（死连接有 EPIPE 兜底，活连接完全没有）。
    //    现在每帧清空 accept 队列只保留最新连接，并按下面规则决定是否接管。
    if (det_listen_fd_ >= 0) {
        int newest = -1;
        for (;;) {
            const int c = ::accept(det_listen_fd_, nullptr, nullptr);
            if (c < 0) break;  // 无更多待处理连接
            const int flags = ::fcntl(c, F_GETFL, 0);
            ::fcntl(c, F_SETFL, flags | O_NONBLOCK);  // 非阻塞写，buffer 满丢帧不阻塞检测
            if (newest >= 0) ::close(newest);
            newest = c;
        }
        if (newest >= 0) {
            const std::uint64_t now_ms = NowMs();
            if (det_conn_fd_ >= 0 && !PeerClosed(det_conn_fd_)
                && (now_ms - det_conn_since_ms_) < kDetHandoverGuardMs) {
                // 刚接管过又有新连接：几乎可以断定是「刚被踢掉的一方在快速重连」。
                // 若也照单接管，两个活进程会每 0.5s 互踢一次、各拿一半帧，比饿死更糟。
                // 短暂忽略，让先接管的一方稳定下来。
                ::close(newest);
            } else {
                if (det_conn_fd_ >= 0) {
                    std::fprintf(stderr,
                                 "[det_socket] %s 接管新判罚连接（旧连接%s），丢弃旧连接\n",
                                 stream_name ? stream_name : "?",
                                 PeerClosed(det_conn_fd_) ? "已断" : "仍活");
                    ::close(det_conn_fd_);
                }
                det_conn_fd_ = newest;
                det_conn_since_ms_ = now_ms;
            }
        }
    }
    if (det_conn_fd_ < 0) return;

    // 2) 序列化「整条消息」到连续 buffer，再原子写出（header 与 balls 不能分开写，
    //    否则中途 EAGAIN 会让对端永久错位，详见 WriteMessageAtomic 注释）。
    //    packed 布局与 Python struct '<IQQI' + 每条 '<fffff' 一一对应。
    static constexpr std::size_t kHdrSize = 24;   // magic(4)+seq(8)+pts_ms(8)+ball_count(4)
    static constexpr std::size_t kBallSize = 20;  // cx,cy,w,h,conf（5×float）
    thread_local std::vector<std::uint8_t> msg;   // thread_local：避免每帧堆分配
    msg.resize(kHdrSize + dets.size() * kBallSize);
    auto put_u32 = [&msg](std::size_t at, std::uint32_t v) { std::memcpy(&msg[at], &v, 4); };
    auto put_u64 = [&msg](std::size_t at, std::uint64_t v) { std::memcpy(&msg[at], &v, 8); };
    auto put_f32 = [&msg](std::size_t at, float v) { std::memcpy(&msg[at], &v, 4); };
    put_u32(0, 0x444C5841U);  // 'AXLD' little-endian
    put_u64(4, seq);
    put_u64(12, pts_ms);
    put_u32(20, static_cast<std::uint32_t>(dets.size()));
    std::size_t off = kHdrSize;
    for (const auto& d : dets) {
        // 与 HTTP 接口对齐：(cx,cy,w,h,conf) = 中心点 + 宽高 + 置信度
        put_f32(off + 0, (d.x0 + d.x1) / 2.0f);
        put_f32(off + 4, (d.y0 + d.y1) / 2.0f);
        put_f32(off + 8, d.x1 - d.x0);
        put_f32(off + 12, d.y1 - d.y0);
        put_f32(off + 16, d.score);
        off += kBallSize;
    }
    // rc == 0：一个字节都没写出去，只是 buffer 满，连接还在 → 只丢这一帧；
    // rc < 0：写了半条或连接已断 → 必须断连，让客户端重连后重新对齐消息边界。
    if (WriteMessageAtomic(det_conn_fd_, msg.data(), msg.size()) < 0) {
        ::close(det_conn_fd_);
        det_conn_fd_ = -1;
    }
}

VideoInfo PipelineInstance::GetVideoInfo() const {
    VideoInfo v;
    v.width = frame_info_->source_w.load(std::memory_order_relaxed);
    v.height = frame_info_->source_h.load(std::memory_order_relaxed);
    if (pipe_) {
        const auto s = pipe_->GetInputStreamInfo();
        v.fps = s.frame_rate;
    }
    return v;
}

bool PipelineInstance::SetOverlay(const axvsdk::common::DrawFrame& osd, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!pipe_) {
        if (error) *error = "pipeline not opened";
        return false;
    }
    if (!pipe_->SetOsd(osd)) {
        if (error) *error = "SetOsd failed";
        return false;
    }
    // 缓存（源图坐标），GetPreviewJpeg 叠加到 preview，让预览画面与判罚同源同步。
    overlay_osd_ = osd;
    has_overlay_ = true;
    return true;
}

void PipelineInstance::StartNpuIfEnabled() {
    if (!pipe_) return;
    if (!cfg_.npu.enable) return;

    ai::AsyncInferOptions nopt{};
    nopt.device_id = cfg_.device_id;
    nopt.plugin_path = cfg_.npu.ax_plugin_path;
    nopt.plugin_init_json = cfg_.npu.ax_plugin_init_json;
    if (cfg_.npu.ax_plugin_isolation == "process") {
        nopt.plugin_isolation = plugin::AxPluginIsolationMode::kSubprocess;
    } else {
        nopt.plugin_isolation = plugin::AxPluginIsolationMode::kInProcess;
    }

    auto worker = std::make_shared<ai::AsyncInfer>(cfg_.npu_max_fps);
    std::string err;
    if (!worker->Init(nopt, &err)) {
        std::cerr << "NPU init failed (ignored): pipeline=" << cfg_.name << " err=" << err << "\n";
        worker.reset();
    }

    std::shared_ptr<tracking::ByteTrack> tracker;
    if (worker && cfg_.npu.enable_tracking) {
        const auto stream = pipe_->GetInputStreamInfo();
        const int fps = stream.frame_rate > 0.0 ? static_cast<int>(std::lround(stream.frame_rate)) : 30;
        tracking::ByteTrackOptions topt{};
        topt.frame_rate = fps > 0 ? fps : 30;
        topt.track_buffer = cfg_.npu.track_buffer > 0 ? static_cast<int>(cfg_.npu.track_buffer) : 30;
        topt.min_score = 0.0F;
        topt.smooth = cfg_.npu.kalman_smooth;
        tracker = std::make_shared<tracking::ByteTrack>(topt);
    }

    if (worker) {
        npu_ok_->store(0, std::memory_order_relaxed);
        npu_err_->store(0, std::memory_order_relaxed);

        // 流帧率（事件队列 pts_ms 换算用）
        int fps = 30;
        if (const auto stream = pipe_->GetInputStreamInfo(); stream.frame_rate > 0.0) {
            fps = static_cast<int>(std::lround(stream.frame_rate));
        }

        auto* pipe_ptr = pipe_.get();
        auto fi = frame_info_;
        const auto resize_opts = cfg_.sdk.frame_output.resize;
        const bool enable_osd = cfg_.npu.enable_osd;

        worker->SetCallbacks(
            [this,
             name = cfg_.name,
             pipe_ptr,
             fi,
             resize_opts,
             enable_osd,
             ok = npu_ok_,
             tracker,
             fps](const std::vector<ai::Detection>& dets_infer, std::uint64_t seq) {
                if (ok) ok->fetch_add(1, std::memory_order_relaxed);

                std::vector<ai::Detection> dets = dets_infer;
                const auto sw = fi->source_w.load(std::memory_order_relaxed);
                const auto sh = fi->source_h.load(std::memory_order_relaxed);
                const auto iw = fi->infer_w.load(std::memory_order_relaxed);
                const auto ih = fi->infer_h.load(std::memory_order_relaxed);
                if (sw != 0 && sh != 0 && iw != 0 && ih != 0) {
                    const auto map = ai::ComputeInferToSourceMap(sw, sh, iw, ih, resize_opts);
                    ai::MapDetectionsInferToSource(map, sw, sh, &dets);
                }

                // 事件队列 + 最新帧（源图坐标，与判罚 H 矩阵同源）
                // StoreLastDetections 内部锁 det_mutex_，不占 mu_（避免与 preview/overlay HTTP 请求抢锁）
                {
                    const auto pts_ms = (fps > 0) ? (seq * 1000ULL / static_cast<std::uint64_t>(fps)) : 0ULL;
                    StoreLastDetections(dets, seq, pts_ms);
                    // 【2026-09-10 检测结果推送】Unix socket 推给判罚进程（替代 HTTP 轮询 drain）
                    PushDetections(dets, seq, pts_ms, name.c_str());
                }

                // 精度对比：dump 检测框（源图坐标）到文件，AXP_DUMP_DETS=1 启用；带 pipeline name 前缀区分多路
                if (EnvFlagEnabled("AXP_DUMP_DETS")) {
                    std::FILE* df = std::fopen("/tmp/ax_dets_dump.txt", "a");
                    if (df) {
                        std::fprintf(df, "%s %llu", name.c_str(), static_cast<unsigned long long>(seq));
                        for (const auto& d : dets) {
                            std::fprintf(df, " %.1f,%.1f,%.1f,%.1f,%.3f", d.x0, d.y0, d.x1, d.y1, d.score);
                        }
                        std::fprintf(df, "\n");
                        std::fclose(df);
                    }
                }

                if (enable_osd && pipe_ptr) {
                    axvsdk::common::DrawFrame osd{};
                    osd.hold_frames = 10;
                    if (sw != 0 && sh != 0) {
                        const bool plugin_has_track_id = std::any_of(
                            dets.begin(), dets.end(), [](const ai::Detection& d) { return d.track_id >= 0; });
                        if (tracker && !plugin_has_track_id) {
                            const auto tracks = tracker->Update(dets);
                            osd.rects.reserve(tracks.size());
                            for (const auto& t : tracks) {
                                float x0f = t.x0;
                                float y0f = t.y0;
                                float x1f = t.x1;
                                float y1f = t.y1;
                                if (x1f < x0f) std::swap(x0f, x1f);
                                if (y1f < y0f) std::swap(y0f, y1f);
                                x0f = std::max(0.0F, std::min(x0f, static_cast<float>(sw - 1)));
                                y0f = std::max(0.0F, std::min(y0f, static_cast<float>(sh - 1)));
                                x1f = std::max(0.0F, std::min(x1f, static_cast<float>(sw - 1)));
                                y1f = std::max(0.0F, std::min(y1f, static_cast<float>(sh - 1)));
                                const auto w = static_cast<std::int32_t>(x1f - x0f);
                                const auto h = static_cast<std::int32_t>(y1f - y0f);
                                if (w <= 1 || h <= 1) continue;
                                axvsdk::common::DrawRect r{};
                                r.x = static_cast<std::int32_t>(x0f);
                                r.y = static_cast<std::int32_t>(y0f);
                                r.width = static_cast<std::uint32_t>(w);
                                r.height = static_cast<std::uint32_t>(h);
                                r.thickness = 2;
                                r.alpha = 255;
                                r.color = tracking::ByteTrack::ColorForTrackId(static_cast<std::uint64_t>(t.track_id));
                                osd.rects.push_back(r);
                            }
                        } else {
                            osd.rects.reserve(dets.size());
                            for (const auto& d : dets) {
                                if (d.score < 0.01F) continue;
                                float x0f = d.x0;
                                float y0f = d.y0;
                                float x1f = d.x1;
                                float y1f = d.y1;
                                if (x1f < x0f) std::swap(x0f, x1f);
                                if (y1f < y0f) std::swap(y0f, y1f);
                                x0f = std::max(0.0F, std::min(x0f, static_cast<float>(sw - 1)));
                                y0f = std::max(0.0F, std::min(y0f, static_cast<float>(sh - 1)));
                                x1f = std::max(0.0F, std::min(x1f, static_cast<float>(sw - 1)));
                                y1f = std::max(0.0F, std::min(y1f, static_cast<float>(sh - 1)));
                                const auto w = static_cast<std::int32_t>(x1f - x0f);
                                const auto h = static_cast<std::int32_t>(y1f - y0f);
                                if (w <= 1 || h <= 1) continue;
                                axvsdk::common::DrawRect r{};
                                r.x = static_cast<std::int32_t>(x0f);
                                r.y = static_cast<std::int32_t>(y0f);
                                r.width = static_cast<std::uint32_t>(w);
                                r.height = static_cast<std::uint32_t>(h);
                                r.thickness = 2;
                                r.alpha = 255;
                                if (d.track_id >= 0) {
                                    r.color = tracking::ByteTrack::ColorForTrackId(static_cast<std::uint64_t>(d.track_id));
                                } else {
                                    r.color = 0x00FF00;
                                }
                                osd.rects.push_back(r);
                            }
                        }
                    }
                    if (!osd.rects.empty()) {
                        (void)pipe_ptr->SetOsd(osd);
                    }
                }

                if (EnvFlagEnabled("AXP_NPU_LOG") && (seq % 30) == 0) {
                    std::cout << "[npu pipeline=" << name << "] seq=" << seq << " dets=" << dets_infer.size() << "\n";
                }
            },
            [name = cfg_.name, errc = npu_err_](const std::string& e, std::uint64_t seq) {
                if (errc) errc->fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[npu pipeline=" << name << "] seq=" << seq << " error=" << e << "\n";
            });
    }

    npu_worker_ = std::move(worker);
    tracker_ = std::move(tracker);
    BuildFrameCallback();
}

std::shared_ptr<ai::AsyncInfer> PipelineInstance::DetachNpuWorkerLocked() noexcept {
    auto worker = std::move(npu_worker_);
    tracker_.reset();
    return worker;
}

void PipelineInstance::ClearOsdIfAny() noexcept {
    if (pipe_) {
        pipe_->ClearOsd();
    }
}

bool PipelineInstance::Open(std::string* error) {
    std::shared_ptr<ai::AsyncInfer> old_worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (pipe_) return true;
        if (!BuildPipeline(error)) return false;
        old_worker = DetachNpuWorkerLocked();
        StartNpuIfEnabled();
    }
    if (old_worker) old_worker->Stop();
    return true;
}

bool PipelineInstance::Start(std::string* error) {
    std::shared_ptr<ai::AsyncInfer> old_worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!pipe_) {
            if (!BuildPipeline(error)) return false;
            old_worker = DetachNpuWorkerLocked();
            StartNpuIfEnabled();
            // 【2026-09-10 检测结果推送】建 Unix socket，供判罚进程 connect 收检测结果
            // 注意：这里已持 mu_ 锁，直接读 cfg_.name（name() 会再锁 mu_ 死锁）
            StartDetectionsSocket(cfg_.name);
        } else if (!npu_worker_) {
            // 【2026-09-11 修复】Stop() 只把 NPU worker 摘掉、不销毁 pipeline
            // （DetachNpuWorkerLocked 把 npu_worker_ 置空），而原来的 Start() 只在
            // 「首次建 pipeline」分支里建 worker → 同一个 pipeline「先 stop 再 start」
            // 之后 NPU 永远不会再推理，检测结果不再推送，判罚静默失效。
            // 这里补上：pipeline 还在但 worker 没了，就重建 worker。
            old_worker = DetachNpuWorkerLocked();   // 正常为 nullptr，防御性清理
            StartNpuIfEnabled();
        }
        if (running_) return true;
        if (!pipe_ || !pipe_->Start()) {
            if (error) *error = "pipeline Start failed";
            return false;
        }
        running_ = true;
    }
    if (old_worker) old_worker->Stop();
    return true;
}

void PipelineInstance::Stop() noexcept {
    std::shared_ptr<ai::AsyncInfer> worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        worker = DetachNpuWorkerLocked();
        if (pipe_) {
            pipe_->Stop();
        }
    }
    if (worker) worker->Stop();
}

void PipelineInstance::Close() noexcept {
    std::shared_ptr<ai::AsyncInfer> worker;
    std::unique_ptr<axvsdk::pipeline::Pipeline> pipe;
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        worker = DetachNpuWorkerLocked();
        pipe = std::move(pipe_);
        {
            std::lock_guard<std::mutex> det_lock(det_mutex_);
            last_dets_.clear();
            last_det_seq_ = 0;
        }
        frame_counter_->store(0, std::memory_order_relaxed);
        frame_info_->source_w.store(0, std::memory_order_relaxed);
        frame_info_->source_h.store(0, std::memory_order_relaxed);
        frame_info_->infer_w.store(0, std::memory_order_relaxed);
        frame_info_->infer_h.store(0, std::memory_order_relaxed);
    }
    if (worker) worker->Stop();
    if (pipe) {
        pipe->Stop();
        pipe->Close();
    }
    // 【2026-09-10 检测结果推送】关闭 Unix socket（判罚进程 recv 返回 0 断开）
    StopDetectionsSocket();
}

bool PipelineInstance::Reconfigure(const ConfigLoader::PipelineCfg& cfg, bool autostart, std::string* error) {
    std::shared_ptr<ai::AsyncInfer> old_worker;
    std::unique_ptr<axvsdk::pipeline::Pipeline> old_pipe;
    ConfigLoader::PipelineCfg old_cfg;
    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        old_cfg = cfg_;
        was_running = running_;

        running_ = false;
        old_worker = DetachNpuWorkerLocked();
        ClearOsdIfAny();
        old_pipe = std::move(pipe_);

        cfg_ = cfg;
        frame_counter_->store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> det_lock(det_mutex_);
            last_dets_.clear();
            last_det_seq_ = 0;
        }
    }

    if (old_worker) old_worker->Stop();
    if (old_pipe) {
        old_pipe->Stop();
        old_pipe->Close();
    }

    std::string open_err;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!BuildPipeline(&open_err)) {
            // Rollback best-effort.
            cfg_ = old_cfg;
            (void)BuildPipeline(nullptr);
            StartNpuIfEnabled();
            if (was_running && pipe_) {
                (void)pipe_->Start();
                running_ = true;
            }
            if (error) *error = open_err.empty() ? "reconfigure open failed" : open_err;
            return false;
        }
        StartNpuIfEnabled();
    }

    if (autostart && was_running) {
        bool started = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (pipe_ && pipe_->Start()) {
                running_ = true;
                started = true;
            }
        }
        if (!started) {
            const std::string start_err = "reconfigure start failed";
            Close();
            {
                std::lock_guard<std::mutex> lock(mu_);
                cfg_ = old_cfg;
            }
            (void)Open(nullptr);
            if (was_running) (void)Start(nullptr);
            if (error) *error = start_err;
            return false;
        }
    }
    return true;
}

bool PipelineInstance::UpdateNpu(const ConfigLoader::PipelineCfg::NpuCfg& npu,
                                double npu_max_fps,
                                bool autostart,
                                std::string* error) {
    std::shared_ptr<ai::AsyncInfer> old_worker;
    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        was_running = running_;
        cfg_.npu = npu;
        cfg_.npu_max_fps = npu_max_fps;
        old_worker = DetachNpuWorkerLocked();
        ClearOsdIfAny();
    }
    // 【2026-09-11 修复】必须先彻底停掉旧 worker（内部 join 线程）再起新的。
    // 原实现是「先 StartNpuIfEnabled() 起新 worker，函数末尾才 Stop() 旧 worker」，
    // 窗口期内新旧两个 worker 的 on_result 会同时往同一个判罚 socket 推帧，
    // 判罚侧看到的 seq 会来回跳（重复/倒序），轨迹与落点被污染。
    // stop 放在锁外：AsyncInfer::Stop 会 join 线程，持 mu_ 时 join 会与 on_result 路径互等。
    if (old_worker) old_worker->Stop();

    bool need_restart = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        StartNpuIfEnabled();
        need_restart = autostart && was_running && pipe_ && !running_;
        if (need_restart) {
            if (!pipe_->Start()) {
                if (error) *error = "pipeline Start failed after npu update";
                return false;
            }
            running_ = true;
        }
    }
    return true;
}

PipelineSnapshot PipelineInstance::Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    PipelineSnapshot s{};
    s.name = cfg_.name;
    s.device_id = cfg_.device_id;
    s.running = running_;
    if (pipe_) {
        s.stats = pipe_->GetStats();
    }
    s.npu_ok = npu_ok_ ? npu_ok_->load(std::memory_order_relaxed) : 0;
    s.npu_err = npu_err_ ? npu_err_->load(std::memory_order_relaxed) : 0;
    return s;
}

bool PipelineInstance::AddOutput(const axvsdk::pipeline::PipelineOutputConfig& output,
                                std::size_t* out_index,
                                std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!pipe_) {
        if (!BuildPipeline(error)) {
            return false;
        }
        StartNpuIfEnabled();
    }

    std::string err;
    std::size_t idx = 0;
    if (!pipe_->AddOutput(output, &idx, &err)) {
        if (error) *error = err.empty() ? "pipeline AddOutput failed" : err;
        return false;
    }

    cfg_.sdk.outputs.push_back(output);
    if (out_index) {
        *out_index = idx;
    }
    return true;
}

bool PipelineInstance::RemoveOutput(std::size_t index, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!pipe_) {
        if (error) *error = "pipeline not opened";
        return false;
    }

    std::string err;
    if (!pipe_->RemoveOutput(index, &err)) {
        if (error) *error = err.empty() ? "pipeline RemoveOutput failed" : err;
        return false;
    }
    if (index < cfg_.sdk.outputs.size()) {
        cfg_.sdk.outputs.erase(cfg_.sdk.outputs.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return true;
}

bool PipelineInstance::GetPreviewJpeg(const PreviewOptions& opt,
                                     std::vector<std::uint8_t>* out_jpeg,
                                     std::string* error) {
    if (out_jpeg == nullptr) return false;
    out_jpeg->clear();

    std::lock_guard<std::mutex> lock(mu_);
    if (!pipe_) {
        if (error) *error = "pipeline not opened";
        return false;
    }

    auto frame = pipe_->GetLatestFrame();
    if (!frame) {
        if (error) *error = "no latest frame";
        return false;
    }

    const std::uint32_t src_w = frame->width();
    const std::uint32_t src_h = frame->height();

    std::uint32_t dst_w = src_w;
    std::uint32_t dst_h = src_h;
    if (opt.max_width > 0 && opt.max_height > 0 && src_w > 0 && src_h > 0) {
        const double sx = static_cast<double>(opt.max_width) / static_cast<double>(src_w);
        const double sy = static_cast<double>(opt.max_height) / static_cast<double>(src_h);
        const double s = std::min(1.0, std::min(sx, sy));
        dst_w = static_cast<std::uint32_t>(std::lround(static_cast<double>(src_w) * s));
        dst_h = static_cast<std::uint32_t>(std::lround(static_cast<double>(src_h) * s));
        // NV12 requires even dimensions.
        dst_w = std::max<std::uint32_t>(2U, AlignEven(dst_w));
        dst_h = std::max<std::uint32_t>(2U, AlignEven(dst_h));
    }

    // Always generate NV12 preview for consistent drawer/JPEG encode behavior.
    // 嵌入式 CMM 不做按帧申请/释放(碎片化风险):buffer/processor 缓存复用,
    // 仅在预览尺寸变化时重建;mutex 串行化并发预览请求(共享同一块缓存)。
    std::lock_guard<std::mutex> preview_lock(preview_mutex_);
    if (!preview_image_ || preview_image_->width() != dst_w || preview_image_->height() != dst_h) {
        axvsdk::common::ImageDescriptor desc{};
        desc.format = axvsdk::common::PixelFormat::kNv12;
        desc.width = dst_w;
        desc.height = dst_h;
        axvsdk::common::ImageAllocationOptions alloc{};
        alloc.memory_type = axvsdk::common::MemoryType::kCmm;
        alloc.cache_mode = axvsdk::common::CacheMode::kNonCached;
        alloc.token = "ax-pipeline-preview";
        preview_image_ = axvsdk::common::AxImage::Create(desc, alloc);
    }
    auto& preview = preview_image_;
    if (!preview) {
        if (error) *error = "alloc preview image failed";
        return false;
    }

    if (!preview_proc_) preview_proc_ = axvsdk::common::CreateImageProcessor();
    auto& proc = preview_proc_;
    if (!proc) {
        if (error) *error = "CreateImageProcessor failed";
        return false;
    }
    axvsdk::common::ImageProcessRequest req{};
    req.output_image = preview->descriptor();
    req.resize.mode = axvsdk::common::ResizeMode::kKeepAspectRatio;
    req.resize.background_color = 0;
    if (!proc->Process(*frame, req, *preview)) {
        if (error) *error = "preview resize/convert failed";
        return false;
    }

    if (opt.with_boxes) {
        auto dets = GetLastDetections();
        if (!dets.empty()) {
            const float sx = (src_w > 0) ? (static_cast<float>(dst_w) / static_cast<float>(src_w)) : 1.0F;
            const float sy = (src_h > 0) ? (static_cast<float>(dst_h) / static_cast<float>(src_h)) : 1.0F;

            axvsdk::common::DrawFrame osd{};
            osd.hold_frames = 1;
            osd.rects.reserve(dets.size());
            for (const auto& d : dets) {
                if (d.score < 0.01F) continue;
                float x0f = d.x0 * sx;
                float y0f = d.y0 * sy;
                float x1f = d.x1 * sx;
                float y1f = d.y1 * sy;
                if (x1f < x0f) std::swap(x0f, x1f);
                if (y1f < y0f) std::swap(y0f, y1f);
                x0f = std::max(0.0F, std::min(x0f, static_cast<float>(dst_w - 1)));
                y0f = std::max(0.0F, std::min(y0f, static_cast<float>(dst_h - 1)));
                x1f = std::max(0.0F, std::min(x1f, static_cast<float>(dst_w - 1)));
                y1f = std::max(0.0F, std::min(y1f, static_cast<float>(dst_h - 1)));
                const auto w = static_cast<std::int32_t>(x1f - x0f);
                const auto h = static_cast<std::int32_t>(y1f - y0f);
                if (w <= 1 || h <= 1) continue;
                axvsdk::common::DrawRect r{};
                r.x = static_cast<std::int32_t>(x0f);
                r.y = static_cast<std::int32_t>(y0f);
                r.width = static_cast<std::uint32_t>(w);
                r.height = static_cast<std::uint32_t>(h);
                r.thickness = 2;
                r.alpha = 255;
                r.color = (d.track_id >= 0) ? tracking::ByteTrack::ColorForTrackId(static_cast<std::uint64_t>(d.track_id))
                                            : 0x00FF00;
                osd.rects.push_back(r);
            }

            if (!osd.rects.empty()) {
                if (!preview_drawer_) preview_drawer_ = axvsdk::common::CreateDrawer();
                if (preview_drawer_) {
                    (void)preview_drawer_->Draw(osd, *preview);
                }
            }
        }
    }

    // 叠加 SetOverlay 的 OSD（球场/轨迹/落点/击球/文字位图），让预览画面与判罚同源同步。
    // overlay_osd_ 是源图坐标，按 dst/src 比例缩放；位图 data 不缩放（预渲染固定尺寸），只缩放位置。
    {
        const axvsdk::common::DrawFrame osd = has_overlay_ ? overlay_osd_ : axvsdk::common::DrawFrame{};
        if (!osd.lines.empty() || !osd.polygons.empty() || !osd.rects.empty() || !osd.bitmaps.empty()) {
            const float sx = (src_w > 0) ? (static_cast<float>(dst_w) / static_cast<float>(src_w)) : 1.0F;
            const float sy = (src_h > 0) ? (static_cast<float>(dst_h) / static_cast<float>(src_h)) : 1.0F;
            axvsdk::common::DrawFrame scaled = osd;
            for (auto& l : scaled.lines) {
                for (auto& p : l.points) {
                    p.x = static_cast<std::int32_t>(std::lround(p.x * sx));
                    p.y = static_cast<std::int32_t>(std::lround(p.y * sy));
                }
            }
            for (auto& pg : scaled.polygons) {
                for (auto& p : pg.points) {
                    p.x = static_cast<std::int32_t>(std::lround(p.x * sx));
                    p.y = static_cast<std::int32_t>(std::lround(p.y * sy));
                }
            }
            for (auto& r : scaled.rects) {
                r.x = static_cast<std::int32_t>(std::lround(r.x * sx));
                r.y = static_cast<std::int32_t>(std::lround(r.y * sy));
                r.width = static_cast<std::uint32_t>(std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(r.width * sx))));
                r.height = static_cast<std::uint32_t>(std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(r.height * sy))));
            }
            for (auto& bm : scaled.bitmaps) {
                bm.dst_x = static_cast<std::uint32_t>(std::lround(bm.dst_x * sx));
                bm.dst_y = static_cast<std::uint32_t>(std::lround(bm.dst_y * sy));
            }
            auto drawer = axvsdk::common::CreateDrawer();
            if (drawer) {
                (void)drawer->Draw(scaled, *preview);
            }
        }
    }

    axvsdk::codec::JpegEncodeOptions jopt{};
    jopt.quality = static_cast<std::uint32_t>(std::max(1, std::min(100, opt.quality)));
    *out_jpeg = axvsdk::codec::EncodeJpegToMemory(*preview, jopt);
    if (out_jpeg->empty()) {
        if (error) *error = "jpeg encode failed";
        return false;
    }
    return true;
}

}  // namespace axpipeline::app
