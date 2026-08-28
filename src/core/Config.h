#pragma once

#include <cstdint>
#include <string>

namespace nr {

enum class CaptureMode {
    FullScreen,
    Region,
    Window,
};

enum class BitrateMode {
    Quality,      // CRF
    FileSize,     // CBR
};

enum class AudioSourceMode {
    SystemOnly,
    MicOnly,
    Both,
};

struct Region {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0; }
};

struct VideoConfig {
    CaptureMode mode = CaptureMode::FullScreen;
    Region region;
    int windowId = 0;           // SCWindow.windowID when mode == Window
    int width = 1920;
    int height = 1080;
    int fps = 60;              // 24 / 30 / 60 / 120, 0 = auto (display refresh)
    bool captureCursor = true;
    bool clickEffects = false;
    std::string codec = "auto"; // auto | h264 | h265
    BitrateMode bitrateMode = BitrateMode::FileSize;
    int bitrateKbps = 6000;    // CBR/VBR target
    int crf = 18;              // quality-first mode
    std::string preset = "medium";
    bool annotationMode = false;  // on-screen annotation overlay while recording
};

struct AudioConfig {
    bool captureSystemAudio = true;
    bool captureMicrophone = false;
    int systemVolume = 100;    // 0-100
    int micVolume = 100;       // 0-100
    bool denoise = false;
    int sampleRate = 48000;
    int channels = 2;
};

struct GeneralConfig {
    std::string outputDir = "~/Movies/NotionRecorder";
    std::string format = "mp4";  // mp4 | mkv
    bool autoSaveNotify = true;
    bool scheduledRecording = false;
    int scheduledDelaySec = 0;    // 0 = start immediately
    int scheduledDurationSec = 0; // 0 = record until stopped manually
};

struct HotkeyConfig {
    std::string startStop = "CmdOrCtrl+Shift+R";
    std::string pause = "CmdOrCtrl+Shift+P";
};

struct RecordingConfig {
    VideoConfig video;
    AudioConfig audio;
    GeneralConfig general;
    HotkeyConfig hotkeys;

    static RecordingConfig defaults();
    std::string defaultFileName() const;
};

} // namespace nr
