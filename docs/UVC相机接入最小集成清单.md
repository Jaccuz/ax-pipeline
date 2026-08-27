# UVC 相机接入 ax_pipeline_app — 最小集成 C++ 改动清单

> 目标：让 ax_pipeline_app 支持 `input_type:"camera"`（UVC 相机 MJPG 直采 + VPU JPEG 硬解 → NV12 → NPU），
> 与现有 mp4/rtsp 共用同一套 NPU 检测（插件）+ OSD + 输出 + HTTP 回传。检测逻辑零改动。
>
> 依据：官方 `stereo_depth`（V4L2Capture）+ `ax-video-sdk`（DecodeJpegMemory）API，不用 ravehun。

---

## 0. 架构结论（为什么改动这么小）

ax_pipeline_app 的帧流模型（`src/app/pipeline_instance.cpp`）：

```
cfg.sdk.uri(mp4/rtsp) → axvsdk::pipeline::CreatePipeline().Open(cfg.sdk)  [demuxer→vdec]
                      → SetFrameCallback( AxImage::Ptr NV12 )             [解码输出帧回调]
                      → NPU worker(AsyncInfer 加载 libax_plugin_yolo26.so) [插件做检测后处理]
                      → detections 存储 + OSD + H264输出 + HTTP回传
```

**camera 只需要替换「demuxer→vdec」这一段**：改成「V4L2 采集 MJPG → DecodeJpegMemory(VPU JPEG硬解) → AxImage::Ptr NV12」，
然后调同一个 frame callback。后面 NPU 插件、OSD、输出、回传**全部复用**。

关键类型匹配：`DecodeJpegMemory(data,size)` 返回 `common::AxImage::Ptr`，`SetFrameCallback` 的回调签名正是 `std::function<void(axvsdk::common::AxImage::Ptr)>`，无需转换。

---

## 1. 复制 V4L2Capture.cpp/hpp（改 MJPG）

来源：`https://github.com/AXERA-TECH/stereo_depth` 的 `V4L2Capture.cpp`(816 行) + `V4L2Capture.hpp`。

- 纯标准 Linux V4L2 MMAP（open / VIDIOC_S_FMT / S_PARM / REQBUFS / mmap / QBUF / STREAMON / DQBUF），**无爱芯依赖**，直接复制进 `src/app/`。
- **改一处**：`V4L2Capture.cpp:635` `fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;` → `V4L2_PIX_FMT_MJPEG;`（我们相机 MJPG，带宽小 + 最高 120fps）。
- 复用它的 `printSupportedModes`（`-l` 列模式）、UVC 控制、`probeDeviceInfo`。

## 2. config_loader（加 camera 配置）

`src/config_loader.*` 的 `PipelineCfg` 加：

```cpp
enum class InputType { kFile, kCamera };
struct CameraCfg { std::string device{"/dev/video0"}; int width=1920; int height=1200; int fps=60; };
// PipelineCfg 加：
InputType input_type = InputType::kFile;   // 默认 file（兼容现有 mp4/rtsp）
CameraCfg camera;
```

JSON 解析（config_loader 里）：读 `"input_type":"camera"` + `"camera":{device,width,height,fps}`。

## 3. pipeline_instance.cpp（核心，两处）

**3a. 保存回调**（`BuildFrameCallback()`，约 81 行）：
把现在直接传给 `pipe_ptr->SetFrameCallback(lambda)` 的 lambda，先存成成员 `std::function<void(axvsdk::common::AxImage::Ptr)> frame_callback_;`，再 `SetFrameCallback(frame_callback_)`。

**3b. camera 分支**（`BuildPipeline()`，约 55 行）：
```cpp
if (cfg_.input_type == ConfigLoader::InputType::kCamera) {
    // 不走 CreatePipeline().Open(cfg.sdk)，改启动 camera 采集线程
    StartCameraCapture();   // 见下
} else {
    auto p = axvsdk::pipeline::CreatePipeline();
    p->Open(cfg_.sdk);  // 现有 mp4/rtsp 路径
    ...
}
```

**3c. camera 采集线程**（新增，`StartCameraCapture()`）：
```cpp
// 线程循环：
V4L2Capture cap(cfg_.camera.device, cfg_.camera.width, cfg_.camera.height, cfg_.camera.fps);
cap.start();
while (!stop) {
    V4L2Capture::Frame f;
    if (!cap.grab(f)) continue;
    // VPU JPEG 硬解 → NV12 AxImage（ax-video-sdk 官方 API）
    auto image = axvsdk::codec::DecodeJpegMemory(f.data, f.size);   // common::AxImage::Ptr
    cap.release(f);
    if (!image) continue;
    frame_callback_(image);   // ← 直接喂现有帧回调，后面 NPU/OSD/输出全复用
}
```

## 4. http_api_server.cpp（AddPipeline 支持 camera）

`AddPipeline` 请求体解析（`src/app/http_api_server.cpp` 约 260 行构造 `ConfigLoader::PipelineCfg` 处）加：
- 读 `"input_type"`（"camera"/缺省 file）、`"camera"` 子对象 → 填 `cfg.input_type` + `cfg.camera`。
- 这样前端 `AxStreamClient.open()` 加个 `input_type`/`camera` 字段就能动态切相机。

## 5. main.cpp（CheckInputUri 支持 camera）

`src/main.cpp` 的 `CheckInputUri`（约 29 行）：`input_type==kCamera` 时**跳过文件存在检查**（相机没有文件路径），直接 return true；camera 设备是否存在交给 V4L2 open 时判断。

---

## 零改动（复用，不动）

- NPU worker `ai::AsyncInfer`（`src/ai/`）—— 加载插件 + 推理
- 检测插件 `libax_plugin_yolo26.so`（`plugins/yolo26/`）—— YOLO 后处理（解码框/NMS/conf）
- OSD 叠加、H264 编码 + RTSP 输出
- `drain_detections` / `detections` 存储 / HTTP 回传（Python 判罚链路无感）

---

## 官方 API 出处（都验证过）

| API | 作用 | 出处 |
|---|---|---|
| `V4L2Capture` | UVC 采集（标准 V4L2 MMAP） | `stereo_depth/V4L2Capture.cpp` |
| `DecodeJpegMemory` | MJPG→NV12 VPU 硬解 | `ax-video-sdk/src/codec/ax_jpeg_codec.cpp#L538` |
| `CreatePipeline().Open` / `SetFrameCallback` | 现有 mp4/rtsp 帧流 | `ax-pipeline/src/app/pipeline_instance.cpp` |

## 验证方式

1. 交叉编译（同 `build_ax650.sh`）→ 新 `ax_pipeline_app`
2. camera config：`{"input_type":"camera","camera":{"device":"/dev/video0","width":1920,"height":1200,"fps":60},"npu":{...同 mp4...},"outputs":[...]}`
3. 日志判据：`v4l2: format set to 1920x1200 fmt=3 fps=60`（MJPEG）+ `[npu pipeline=cam0] seq=N dets=N`（NPU 检测）+ `[pipeline_frame] frame=N`（帧回调在跑）
4. `drain_detections` 能拿到 `dets`（有球时）或空（无球时），HTTP 回传正常 = 集成成功
