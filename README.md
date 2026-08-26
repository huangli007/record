# NotionRecorder — 极简录屏软件（C++）

根据《架构设计文档》与《C++ 录屏软件产品需求文档（PRD V1.0）》开发的高性能桌面录屏工具原型。当前为 macOS 平台实现（Windows 的 DXGI/WASAPI 路径按 PRD 预留接口，后续可按同一抽象层接入）。

## 特性（当前版本）

- **四线程生产者-消费者架构**：采集 → 原始队列 → 编码线程 → 包队列 → 封装线程 → 文件，各模块互不阻塞
- **屏幕采集**：macOS ScreenCaptureKit，支持全屏与区域录制、光标捕获、帧率锁定（24/30/60/120 或跟随屏幕刷新率）
- **音频采集**：系统声音（ScreenCaptureKit Audio）+ 麦克风（CoreAudio AUHAL），双路混音，独立音量控制
- **编码**：FFmpeg，优先硬件编码（`h264_videotoolbox` / `hevc_videotoolbox`），自动回退 `libx264` / `libx265`
- **码率控制**：质量优先（CRF）/ 文件大小优先（CBR）两种模式，默认 1080p 60fps 6000kbps
- **封装**：fragmented MP4（`frag_keyframe+empty_moov`），录制中断/崩溃后已录制部分仍可播放；支持 MKV
- **时间基准**：统一微秒单调时钟，音视频时间戳对齐（PRD 50ms 容差目标）
- **UI**：Qt 6 QML，Notion 极简风格——悬浮控制条（始终置顶）、区域选择遮罩、四栏设置弹窗（通用/视频/音频/热键）
- **状态机**：空闲 → 录制 → 暂停 → 停止 → 保存，暂停不计入时长

## 环境要求

- macOS 13.0+（ScreenCaptureKit）
- CMake ≥ 3.21、Qt 6.4+、FFmpeg（`brew install cmake qt ffmpeg`）

## 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
open build/NotionRecorder.app
```

首次运行需要在 **系统设置 → 隐私与安全性 → 屏幕录制** 中授权；录制麦克风时需要 **麦克风** 授权。

## 管线自测（无需录屏/麦克风权限）

`pipeline_test` 用合成帧（渐变画面 + 440Hz 正弦音频）走完整编码/封装链路，产出可播放的 MP4：

```bash
cmake --build build --target pipeline_test -j
./build/pipeline_test /tmp/nr_pipeline_test.mp4
ffprobe /tmp/nr_pipeline_test.mp4   # 应显示 h264 + aac 两个流
```

验证要点：视频流 640x360 @ 30fps 时长 3.0s，音频流 48kHz 双声道 3.008s，音视频时间戳对齐。

## 项目结构

```
src/
├── main.cpp                  # 应用入口（QML 引擎 + AppController 桥接）
├── core/                     # 与平台无关的核心
│   ├── Config.h              # 录制配置（视频/音频/通用/热键）
│   ├── RecordingSession.*    # 状态机 + 四线程管线编排
│   ├── ThreadSafeQueue.h     # 有界线程安全队列（push/pop/unshift/close）
│   └── TimeBase.h            # 统一微秒单调时钟
├── capture/                  # 采集抽象层
│   ├── ScreenCapturer.h      # 屏幕采集接口（Windows 可在此接入 DXGI）
│   ├── AudioCapturer.h       # 音频采集接口（Windows 可接入 WASAPI）
│   └── macos/
│       ├── ScreenCaptureKitCapturer.*  # 屏幕 + 系统声音
│       └── CoreAudioCapturer.*         # 麦克风
├── codec/                    # FFmpeg 编码
│   ├── VideoEncoder.*        # 硬件/软件视频编码（videotoolbox → x264/x265）
│   ├── AudioEncoder.*        # AAC
│   └── EncoderFactory.*      # 编码器选择
├── mux/                      # Muxer.* 封装（fragmented MP4 / MKV）
└── ui/
    ├── AppController.*       # C++ ↔ QML 桥接
    └── qml/                  # Notion 风格界面
        ├── Main.qml          # 悬浮控制条
        ├── RegionSelector.qml
        └── SettingsDialog.qml
```

## 设计说明

### 数据流

```
ScreenCaptureKit（视频 + 系统音频）──┐
                                   ├→ 原始队列 → 编码线程 → 包队列 → 封装线程 → MP4
CoreAudio（麦克风）────────────────┘
```

采集回调查看于平台线程；编码与封装各占一个线程；控制（状态机）运行在 UI 线程。

### 时间戳同步

所有帧使用统一微秒时间线（`TimeBase`）。ScreenCaptureKit 帧以首帧为基准归零，麦克风以采样计数换算，两路音频在混音线程按 40ms 窗口对齐后混合。封装层以 `1/1e6` 作为流 time_base，pts/dts 直接透传，保证音画同步精度。

### 暂停语义

暂停时采集侧丢弃帧（不产生新数据），编码/封装线程继续排空队列；恢复后时间线自然衔接，暂停时长不计入录制时长。

### 性能监控

悬浮条工具提示显示当前编码器名称；`AppController` 每 250ms 轮询采集队列与编码队列深度，可扩展为 PRD 中的 CPU/GPU 占用提示。

## 当前限制与后续路线

- 区域录制当前以 1x 缩放捕获（Retina 屏会降低分辨率），后续可改进为按屏幕缩放比捕获
- 全局热键、点击特效、WebRTC 降噪、实时标注、任务管理、云端上传为后续版本
- Windows DXGI/WASAPI/NVENC/QSV/AMF 路径已预留接口，未实现
- 分辨率变化/音频设备插拔的自动恢复逻辑（PRD 关键实现要点）尚未完全接入，当前 SCK 异常停止会触发错误提示

## 文档对照

| 架构文档章节 | 实现状态 |
| --- | --- |
| 模块拓扑（生产者-消费者） | ✅ 四线程队列管线 |
| 视频采集（全屏/区域/光标） | ✅ ScreenCaptureKit |
| 音频采集（系统+麦克风混音） | ✅ SCK Audio + CoreAudio |
| 编码（硬件优先 + CRF/CBR） | ✅ videotoolbox / x264 |
| 封装（fragmented MP4） | ✅ |
| UI（悬浮条/设置/区域选择） | ✅ Qt 6 QML |
| 性能监控 | 🟡 队列深度（CPU/GPU 占用待接入） |
| 窗口锁定 / 点击特效 / 热键 / 降噪 | ⏳ 后续版本 |
