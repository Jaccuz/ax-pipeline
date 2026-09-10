# 改动记录（Changelog）

## 2026-09-11 — 四路 1080p60 判罚稳定性修复（socket 推送 + 可见几何）

### 背景

AX650N 盒子四路 1080p60 网球判罚，链路为：VDEC 硬解 → NPU YOLO 检测 → 检测结果经 Unix socket 推给每路独立 Python 判罚进程。此前存在两个问题：

1. **判罚侧 recv 掉帧**：四路总有一路 recv 掉到 45fps（且每次掉的路都轮换），根因是 socket 推送「写半条消息不 close」导致字节流错位。
2. **VENC 定尺用 coded 尺寸**：1080p 源（coded 1920×1088、可见 1920×1080）预览编码用错尺寸，之前用「结构体加字段」修复反而触发 ABI 破坏 SIGSEGV。

### 改动范围

**app 侧（`src/app/pipeline_instance.cpp` / `include/app/pipeline_instance.hpp`）**

| 改动 | 说明 |
|---|---|
| `WriteMessageAtomic` | 检测结果整条消息拼连续 buffer 原子写；写半条时 close 断连，杜绝「header+ball 分开写、EAGAIN 不 close」导致的字节流错位 |
| `PeerClosed` | MSG_PEEK 探测对端死活 |
| accept 接管 | backlog 1→8；每帧清空 accept 队列保留最新连接；2s 接管保护窗；防新旧判罚进程互踢 |
| `Start()` 补 worker 重建 | 修「stop→start 后 NPU 不再推理、检测结果不再推送」 |
| `UpdateNpu()` 锁外 stop 旧 worker | 防新旧 worker 同时推 socket 致 seq 跳变 |
| `GetLastDetections` 锁语义澄清 | 防潜在自死锁 |

**lib 侧（`deps/ax-video-sdk` submodule，ABI 安全）**

| 文件 | 改动 |
|---|---|
| `include/codec/ax_codec_types.h` | `VideoStreamInfo` 恢复 24 字节（回退此前 +2 字段的破坏） |
| `include/pipeline/ax_demuxer.h` | `Demuxer` 基类加非虚内联 `visible_width()/visible_height()` |
| `src/pipeline/ax_demuxer.cpp` | SPS/VPS 裁剪窗口解析 → 可见几何（H264 frame_cropping / H265 conformance_window） |
| `src/pipeline/ax_pipeline.cpp` | `ResolveOutputSize(coded, visible, requested)`，VENC 定尺缺省用可见几何 |

关键点：不改 vtable、不改结构体布局 ⇒ **app 无需重编即可替换 lib**。

### 验收结果（2026-09-11）

- 四路 90s e2e：npu_ok ≈56、recv ≈56，四路全部 ≥55fps，收/推 ≈99~100%。
- 6 分钟长稳：recv `56.5 / 55.3 / 55.9 / 56.9`，零掉帧，`web.log` 协议错位 = 0，无内存泄漏。

### 部署状态

- lib：`libax_video_sdk.so`（md5 `a07016c4…`，new2 ABI-safe）
- app：`ax_pipeline_app`（md5 `41bf8e…`，含 socket 推送修复）
- 判罚进程绑核 4/5/6/7，app 不绑核（0-7）。
