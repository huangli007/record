# NotionRecorder — 极简录屏软件（C++）

根据《架构设计文档》与《C++ 录屏软件产品需求文档（PRD V1.0）》开发的高性能桌面录屏工具原型。当前为 macOS 平台实现（Windows 的 DXGI/WASAPI 路径按 PRD 预留接口，后续可按同一抽象层接入）。

## 特性（当前版本）

- **四线程生产者-消费者架构**：采集 → 原始队列 → 编码线程 → 包队列 → 封装线程 → 文件，各模块互不阻塞
- **屏幕采集**：macOS ScreenCaptureKit，支持全屏、区域与**窗口锁定**录制（跟随窗口移动）、光标捕获、帧率锁定（24/30/60/120 或跟随屏幕刷新率）
- **音频采集**：系统声音（ScreenCaptureKit Audio）+ 麦克风（CoreAudio AUHAL），双路混音，独立音量控制
- **编码**：FFmpeg，优先硬件编码（`h264_videotoolbox` / `hevc_videotoolbox`），自动回退 `libx264` / `libx265`
- **分辨率设置**：全屏录制支持 自动（跟随屏幕）/ 720p / 1080p / 1440p / 4K 预设输出，
  区域/窗口录制按选择区域或窗口尺寸
- **码率控制**：质量优先（CRF）/ 文件大小优先（CBR）两种模式，默认 1080p 60fps 6000kbps
- **封装**：fragmented MP4（`frag_keyframe+empty_moov`），录制中断/崩溃后已录制部分仍可播放；支持 MKV
- **时间基准**：统一微秒单调时钟，音视频时间戳对齐（PRD 50ms 容差目标）
- **UI**：Qt 6 QML，Notion 极简风格——悬浮控制条（始终置顶）、区域选择遮罩、四栏设置弹窗（通用/视频/音频/热键）
- **状态机**：空闲 → 录制 → 暂停 → 停止 → 保存，暂停不计入时长
- **全局热键**：⌘⇧R 开始/停止、⌘⇧P 暂停/继续（Carbon EventHotKey，无需辅助功能权限）
- **性能监控**：录制中悬浮条实时显示 CPU 占用率，≥80% 时红色提示（PRD 性能监控指标）
- **保存通知**：录制结束/出错自动弹出 Toast 提示与文件路径
- **动态分辨率适配**：录制中窗口缩放/屏幕分辨率变化时自动缩放编码，不中断录制（PRD 关键实现要点）
- **自动恢复**：麦克风/耳机插拔自动切换采集源（CoreAudio 设备监听）；屏幕分辨率/显示器变化与系统唤醒后自动重建 SCK 采集流
- **Retina 区域录制**：区域按像素分辨率输出（不再降为 1x 点分辨率）
- **高负载提示**：CPU ≥80% 时悬浮条红色提示，点击一键将帧率降至 30FPS
- **设置持久化**：输出目录、录制模式、帧率、码率、音频等配置通过 QSettings 保存，重启后保留
- **麦克风降噪**：实时谱减法降噪（Accelerate vDSP FFT，噪声门 + 非对称增益平滑），
  设置弹窗「音频」页开关；合成测试中稳态噪声衰减 25dB、信号保真 1.03×、
  信噪比提升 16.4dB
- **实时标注**：录制中可开启全屏标注层——画笔、箭头、高亮、文字（Enter 提交），
  6 色可选、撤销/清空；标注直接绘制在屏幕上，随录屏一并捕获
- **自窗口排除**：录制自动排除悬浮条/设置等自身窗口（SCK 按进程过滤），
  标注层按标题保留在画面中
- **定时录制**：设置「通用 → 定时录制」可配置延迟开始（秒）与自动停止时长（秒）；
  点击录制按钮后悬浮条显示倒计时，到点自动开始/停止
- **录制历史**：悬浮条「≡」打开历史面板，按时间列出最近 200 条录制文件
  （文件名/大小/时间），点击直接播放，可刷新或打开输出目录

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

**窗口录制**：点击悬浮条的区域按钮，在遮罩顶部切换到「窗口」模式，从列表选择目标窗口即可；
录制期间窗口移动会被自动跟随，缩放时画面自动适配到初始分辨率。

QML 模块在构建时自动部署到 app bundle 的 `Resources/qml/`，直接运行
`build/NotionRecorder.app` 即可，无需额外设置导入路径。

## 打包分发

```bash
scripts/package_macos.sh          # 默认打包 build/NotionRecorder.app
```

脚本会依次：调用 `macdeployqt` 打包 Qt 框架与 QML 插件 → 递归收集并复制
FFmpeg 及其 Homebrew 传递依赖到 `Contents/Frameworks` → 用
`install_name_tool` 改链为 `@executable_path` / `@loader_path` → ad-hoc
签名并校验 → 输出 `dist/NotionRecorder-0.1.0-macos.zip`（约 190MB，自包含）。

注意：ad-hoc 签名未通过 Apple 公证，他人收到后首次打开需右键 →「打开」，
或执行 `xattr -dr com.apple.quarantine NotionRecorder.app`。

## 管线自测（无需录屏/麦克风权限）

`pipeline_test` 用合成帧（渐变画面 + 440Hz 正弦音频）走完整编码/封装链路，产出可播放的 MP4：

```bash
cmake --build build --target pipeline_test -j
./build/pipeline_test /tmp/nr_pipeline_test.mp4
ffprobe /tmp/nr_pipeline_test.mp4   # 应显示 h264 + aac 两个流
```

验证要点：视频流 640x360 @ 30fps 时长 3.0s，音频流 48kHz 双声道 3.008s，音视频时间戳对齐。
测试末尾额外编码 5 帧 480x270 的异尺寸帧，验证窗口缩放时的硬件缩放路径。

## 降噪自测

```bash
cmake --build build --target denoise_test -j
./build/denoise_test
```

三项断言：① 纯白噪声衰减 ≥12dB（实测 25dB）；② 1kHz 纯音通过后幅度保持
0.5×~1.5×（实测 1.03×）；③ 音+噪输入 SNR 提升 ≥8dB（实测 +16.4dB）。
降噪算法延迟约 1023 采样（21ms@48kHz），在 PRD 50ms 音画同步容差内。

## 实时标注

录制中点击悬浮条「✎」或先在设置「视频 → 实时标注」开启；标注层出现后选择工具
（画笔/箭头/高亮/文字）、颜色，拖拽绘制。Esc 或标注层工具栏「◼」关闭。
标注绘制在屏幕上，由 ScreenCaptureKit 直接捕获，无需在帧上合成。
已知限制：录制中打开设置窗口会被一并捕获（排除列表在流启动时确定）。

## 任务管理

「通用」页的定时录制支持延迟开始（倒计时显示在悬浮条状态区）与定时自动停止；
每次录制结束后自动刷新历史列表。历史面板（悬浮条 ≡）按修改时间倒序展示
输出目录中的 MP4/MKV 文件。

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

悬浮条工具提示显示当前编码器名称；`AppController` 每 250ms 轮询采集/编码队列深度，
并通过 `host_processor_info` 计算 CPU 占用率（增量采样）。录制中 CPU ≥ 80% 时显示红色
提示；GPU 占用监测与"自动降参数建议"留待后续版本。

### 全局热键

使用 Carbon `RegisterEventHotKey` 注册 ⌘⇧R（开始/停止）与 ⌘⇧P（暂停/继续），
无需辅助功能权限即可全局响应。热键定义集中在 `AppController::setupHotkeys()`，
后续可接入设置弹窗中的热键配置项。

## 当前限制与后续路线

- 任务管理、云端上传为后续版本；降噪为轻量谱减法实现（非完整 WebRTC NS）
- Windows DXGI/WASAPI/NVENC/QSV/AMF 路径已预留接口，未实现
- SCK 异常停止（非显示变化原因）会触发错误提示；音频设备插拔与显示变化已自动恢复

## 已修复的兼容性问题

- **录制无声（macOS 14.4+ 系统音频权限差异）**：ScreenCaptureKit 在某些权限
  组合下不会回调任何音频缓冲，导致文件只有音轨头部、没有任何声音。现在若
  采集启动后 2 秒内 SCK 未交付任何音频缓冲，会自动切换到 CoreAudio 全局
  系统音频 Tap（macOS 14.2+）继续采集，录制不再无声。
- **FFmpeg 9.x videotoolbox 崩溃**：FFmpeg 9 的 `videotoolboxenc.c` 从
  `frame->data[3]` 读取 CVPixelBuffer（旧版本读 `data[0]`），编码前需同时填充
  `data[0]` 与 `data[3]`，否则断言崩溃。
- **QML 模块部署**：`qt_finalize_executable` 后仍需将生成的模块目录拷贝进
  app bundle 的 `Resources/qml/`，`main.cpp` 同时显式添加该导入路径，保证
  从构建目录直接运行。

## 音频诊断

每次录制结束后，程序会在录制目录下追加写入 `audio_debug.log`，记录
SCK 音频回调数、Tap 兜底是否启用、混音/编码/封装各环节的帧计数与失败数。
若再次遇到无声问题，把该文件内容发来即可快速定位是哪一环丢失了音频。

## Windows 版本

Windows 平台层（DXGI/GDI 屏幕采集、WASAPI 系统声音回环与麦克风、NVENC/QSV/AMF
硬件编码、Ctrl+Shift+R/P 全局热键、可移植 FFT 降噪）已实现，并通过 CMake 的
`WIN32` 分支接入。本机为 macOS，无法直接产出 Windows 安装包；仓库内附
`.github/workflows/build-release.yml`：推送到 GitHub 后，Actions 会在
Windows 服务器上自动编译并产出 exe 压缩包，打 `v*` 标签时自动发布
Windows 与 macOS 两个平台的包到 Releases。

## 文档对照

| 架构文档章节 | 实现状态 |
| --- | --- |
| 模块拓扑（生产者-消费者） | ✅ 四线程队列管线 |
| 视频采集（全屏/区域/窗口锁定/光标） | ✅ ScreenCaptureKit |
| 音频采集（系统+麦克风混音） | ✅ SCK Audio + CoreAudio |
| 编码（硬件优先 + CRF/CBR） | ✅ videotoolbox / x264 |
| 封装（fragmented MP4） | ✅ |
| UI（悬浮条/设置/区域选择） | ✅ Qt 6 QML |
| 性能监控 | ✅ CPU 占用 + 队列深度（GPU 待接入） |
| 全局热键 | ✅ ⌘⇧R / ⌘⇧P |
| 窗口锁定 | ✅ 跟随窗口移动 + 动态缩放 |
| 设备/显示变化自动恢复 | ✅ 麦克风插拔 + 分辨率/唤醒重建流 |
| Retina 区域像素输出 | ✅ 按背光缩放比输出 |
| 降噪 | ✅ 谱减法实时降噪（vDSP） |
| 实时标注 | ✅ 画笔/箭头/高亮/文字 + 撤销/清空 |
| 任务管理 | ✅ 定时启停 + 历史记录列表（批量导出/队列待后续） |
