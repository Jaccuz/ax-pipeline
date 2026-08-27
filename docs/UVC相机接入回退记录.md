# UVC 相机接入 ax_pipeline_app — 回退记录（2026-08-27）

## 目标

让 ax_pipeline_app 支持 `input_type:"camera"`（UVC 相机 MJPG 直采 + VPU JPEG 硬解 → NV12 → NPU），与 mp4/rtsp 共用检测插件 + OSD + HTTP 回传。

## 改动内容（5 个已跟踪文件 + 3 个新文件）

1. `include/app/V4L2Capture.hpp` + `src/app/V4L2Capture.cpp`（新增，从官方 stereo_depth 复制，改 MJPG）
2. `include/config_loader.hpp`：加 `InputType{kFile,kCamera}` + `CameraCfg` + `input_type`/`camera` 字段解析
3. `include/app/pipeline_instance.hpp`：加 `frame_callback_` + `camera_thread_` + `StartCameraCapture()`
4. `src/app/pipeline_instance.cpp`：BuildPipeline camera 分支 + BuildFrameCallback 保存回调 + StartCameraCapture + Start/Stop/Close/StartNpuIfEnabled camera 兼容
5. `src/main.cpp`：CheckInputUri 对 camera 跳过文件检查
6. `CMakeLists.txt`：加 `src/app/V4L2Capture.cpp`
7. `configs/camera_cam0_official.json`（官方格式 camera config）

## 验证进展（已跑通的部分）

- ✅ 交叉编译通过（arm64，含 V4L2Capture）
- ✅ V4L2 MJPG 采集 60fps（`[camera] capturing /dev/video0 1920x1200@60 MJPEG`）
- ✅ VPU JPEG 硬解成功（`DecodeJpegMemory` → NV12 AxImage，`fmt=1` 有物理地址）
- ✅ 帧回调触发（`[pipeline_frame] frame=N` 每 30 帧，60fps）
- ✅ 官方 mp4/rtsp 检测不受影响（向后兼容）

## 卡点（未解决，根因未定位）

**npu_ok=0**：帧回调正常、Submit 执行（加了「npu_worker null」诊断日志，未打印，说明 npu_worker 非 null），但 `AsyncInfer::ThreadMain` 没推理（无 `[async_infer]` 日志、无 `[npu pipeline]` 检测日志、npu_ok/npu_err 都 0）。

排查过程：
- ❌ 排除 NPU 占用（停官方 ax_pipeline_app 后仍 npu_ok=0）
- ❌ 排除 FpsController Throttle 卡住（`max_fps<=0` 直接 return）
- ❌ 排除 npu_worker 为 null（诊断日志未触发）
- ⚠️ 疑似：Open + Start 都调 StartNpuIfEnabled，worker 重复 Init / 生命周期问题；或 camera 帧回调与 SetFrameCallback 的时序差异。**未定位到根因。**

## 回退决定

用户要求：今天 camera 集成改动**不合入正式代码**，盒子回退到昨天能跑的版本（官方 ax_pipeline_app + 官方 libax_video_sdk.so），清理临时文件。

## 下一步（若重启此任务）

1. 定位 npu_ok=0 根因：重点查 Open→Start 的 worker 重复 Init 时序 + camera 帧回调的 Submit 是否与 AsyncInfer 线程正确联动（加 `[async_infer]` 日志到 ThreadMain 开头确认是否被唤醒）。
2. 参考之前 ravehun 跑通的日志（`[npu pipeline=cam0] seq=N dets=0`）对比差异。
