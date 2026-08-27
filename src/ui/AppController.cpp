#include "ui/AppController.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>

#include <cmath>

#if defined(__APPLE__)
#include <Carbon/Carbon.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#endif

namespace nr {

namespace {

QString formatElapsed(int64_t microseconds) {
    const int64_t totalSeconds = microseconds / 1'000'000;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds % 3600) / 60;
    const int64_t seconds = totalSeconds % 60;
    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QString("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

#if defined(__APPLE__)
enum {
    kHotKeyToggleRecording = 1,
    kHotKeyTogglePause = 2,
};

// Called on the main event loop when a registered global hotkey is pressed.
OSStatus hotKeyEventHandler(EventHandlerCallRef, EventRef event, void* userData) {
    EventHotKeyID hotKeyId{};
    GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                      sizeof(hotKeyId), nullptr, &hotKeyId);
    auto* controller = static_cast<AppController*>(userData);
    if (hotKeyId.id == kHotKeyToggleRecording) {
        controller->toggleRecording();
    } else if (hotKeyId.id == kHotKeyTogglePause) {
        controller->togglePause();
    }
    return noErr;
}

// Cumulative CPU ticks since boot; caller keeps previous values for the delta.
bool readCpuTicks(long long& user, long long& system, long long& idle) {
    processor_info_array_t cpuInfo = nullptr;
    mach_msg_type_number_t numCpuInfo = 0;
    natural_t numCpus = 0;
    const kern_return_t status = host_processor_info(
        mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &numCpus, &cpuInfo, &numCpuInfo);
    if (status != KERN_SUCCESS || !cpuInfo) {
        return false;
    }
    auto* info = reinterpret_cast<processor_cpu_load_info_t>(cpuInfo);
    user = 0;
    system = 0;
    idle = 0;
    for (natural_t i = 0; i < numCpus; ++i) {
        user += info[i].cpu_ticks[CPU_STATE_USER];
        system += info[i].cpu_ticks[CPU_STATE_SYSTEM];
        idle += info[i].cpu_ticks[CPU_STATE_IDLE];
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(cpuInfo),
                  numCpuInfo * sizeof(processor_cpu_load_info));
    return true;
}
#endif

} // namespace

AppController::AppController(QObject* parent)
    : QObject(parent), config_(RecordingConfig::defaults()) {
    connect(&statsTimer_, &QTimer::timeout, this, &AppController::refreshStats);
    statsTimer_.setInterval(250);
#if defined(__APPLE__)
    setupHotkeys();
#endif
}

AppController::~AppController() {
#if defined(__APPLE__)
    if (hotKeyToggleRef_) {
        UnregisterEventHotKey(reinterpret_cast<EventHotKeyRef>(hotKeyToggleRef_));
    }
    if (hotKeyPauseRef_) {
        UnregisterEventHotKey(reinterpret_cast<EventHotKeyRef>(hotKeyPauseRef_));
    }
    if (hotKeyEventHandlerRef_) {
        RemoveEventHandler(reinterpret_cast<EventHandlerRef>(hotKeyEventHandlerRef_));
    }
#endif
}

QString AppController::statusText() const {
    if (errorMessage_.isEmpty() && session_ && session_->state() == RecordingSession::State::Error) {
        return "录制出错";
    }
    if (!session_) {
        return "准备就绪";
    }
    switch (session_->state()) {
        case RecordingSession::State::Recording:
            return "录制中 " + elapsedText();
        case RecordingSession::State::Paused:
            return "已暂停 " + elapsedText();
        case RecordingSession::State::Stopping:
            return "正在保存…";
        default:
            return "准备就绪";
    }
}

bool AppController::isRecording() const {
    return session_ && session_->state() == RecordingSession::State::Recording;
}

bool AppController::isPaused() const {
    return session_ && session_->state() == RecordingSession::State::Paused;
}

QString AppController::elapsedText() const {
    return session_ ? formatElapsed(session_->elapsedUs()) : "00:00";
}

QString AppController::encoderName() const {
    return session_ ? QString::fromStdString(session_->encoderName()) : QString();
}

int AppController::inputQueueDepth() const {
    return session_ ? static_cast<int>(session_->videoQueueDepth()) : 0;
}

int AppController::outputQueueDepth() const {
    return session_ ? static_cast<int>(session_->encodedQueueDepth()) : 0;
}

int AppController::cpuPercent() const {
    return cpuPercent_;
}

bool AppController::highLoad() const {
    return isRecording() && cpuPercent_ >= 80;
}

QString AppController::lastFilePath() const {
    return lastFilePath_;
}

QString AppController::errorMessage() const {
    return errorMessage_;
}

void AppController::toggleRecording() {
    if (session_ && (session_->state() == RecordingSession::State::Recording ||
                     session_->state() == RecordingSession::State::Paused)) {
        stopRecording();
    } else if (!session_ || session_->state() == RecordingSession::State::Idle) {
        startRecording();
    }
}

void AppController::togglePause() {
    if (!session_) {
        return;
    }
    if (session_->state() == RecordingSession::State::Recording) {
        session_->pause();
    } else if (session_->state() == RecordingSession::State::Paused) {
        session_->resume();
    }
}

void AppController::startRecording() {
    if (session_ && session_->state() != RecordingSession::State::Idle) {
        return;
    }
    errorMessage_.clear();
    session_ = std::make_unique<RecordingSession>();
    if (!session_->start(config_, [this](RecordingSession::State state) {
            handleState(state);
        })) {
        if (errorMessage_.isEmpty()) {
            errorMessage_ = QString::fromStdString(session_->lastError());
            Q_EMIT recordingFailed(errorMessage_);
        }
        session_.reset();
        Q_EMIT stateChanged();
        return;
    }
    statsTimer_.start();
}

void AppController::stopRecording() {
    if (!session_) {
        return;
    }
    statsTimer_.stop();
    session_->stop();
    session_.reset();
    Q_EMIT stateChanged();
}

void AppController::handleState(RecordingSession::State state) {
    switch (state) {
        case RecordingSession::State::Recording:
            Q_EMIT stateChanged();
            break;
        case RecordingSession::State::Paused:
            Q_EMIT stateChanged();
            break;
        case RecordingSession::State::Idle:
            if (session_) {
                lastFilePath_ = QString::fromStdString(session_->outputPath());
                if (!lastFilePath_.isEmpty()) {
                    Q_EMIT sessionFinished(lastFilePath_);
                }
            }
            Q_EMIT stateChanged();
            break;
        case RecordingSession::State::Error:
            errorMessage_ = session_ ? QString::fromStdString(session_->lastError())
                                     : QString("未知错误");
            Q_EMIT recordingFailed(errorMessage_);
            Q_EMIT stateChanged();
            break;
        default:
            break;
    }
}

void AppController::refreshStats() {
    if (session_) {
#if defined(__APPLE__)
        long long user = 0, system = 0, idle = 0;
        if (readCpuTicks(user, system, idle)) {
            const long long totalDelta =
                (user - lastUserTicks_) + (system - lastSystemTicks_) + (idle - lastIdleTicks_);
            if (lastUserTicks_ != 0 && totalDelta > 0) {
                cpuPercent_ = static_cast<int>(std::lround(
                    100.0 * ((user - lastUserTicks_) + (system - lastSystemTicks_)) /
                    static_cast<double>(totalDelta)));
            }
            lastUserTicks_ = user;
            lastSystemTicks_ = system;
            lastIdleTicks_ = idle;
        }
#endif
        Q_EMIT statsChanged();
        Q_EMIT elapsedChanged();
    }
}

#if defined(__APPLE__)
void AppController::setupHotkeys() {
    EventTypeSpec eventType{kEventClassKeyboard, kEventHotKeyPressed};
    InstallEventHandler(GetEventDispatcherTarget(), hotKeyEventHandler, 1, &eventType,
                        this, reinterpret_cast<EventHandlerRef*>(&hotKeyEventHandlerRef_));

    EventHotKeyID hotKeyId{'NRHK', kHotKeyToggleRecording};
    RegisterEventHotKey(kVK_ANSI_R, cmdKey | shiftKey, hotKeyId,
                        GetEventDispatcherTarget(), 0,
                        reinterpret_cast<EventHotKeyRef*>(&hotKeyToggleRef_));

    hotKeyId.id = kHotKeyTogglePause;
    RegisterEventHotKey(kVK_ANSI_P, cmdKey | shiftKey, hotKeyId,
                        GetEventDispatcherTarget(), 0,
                        reinterpret_cast<EventHotKeyRef*>(&hotKeyPauseRef_));
}
#else
void AppController::setupHotkeys() {}
#endif

void AppController::setRegion(int x, int y, int width, int height) {
    config_.video.mode = CaptureMode::Region;
    config_.video.region = Region{x, y, width, height};
    config_.video.width = width;
    config_.video.height = height;
}

void AppController::setCaptureMode(int mode) {
    config_.video.mode = mode == 1 ? CaptureMode::Region : CaptureMode::FullScreen;
}

void AppController::setFps(int fps) {
    config_.video.fps = fps;
}

void AppController::setCaptureCursor(bool enabled) {
    config_.video.captureCursor = enabled;
}

void AppController::setClickEffects(bool enabled) {
    config_.video.clickEffects = enabled;
}

void AppController::setCodec(const QString& codec) {
    config_.video.codec = codec.toStdString();
}

void AppController::setBitrateMode(int mode) {
    config_.video.bitrateMode = mode == 0 ? BitrateMode::Quality : BitrateMode::FileSize;
}

void AppController::setBitrateKbps(int kbps) {
    config_.video.bitrateKbps = kbps;
}

void AppController::setCrf(int crf) {
    config_.video.crf = crf;
}

void AppController::setSystemAudio(bool enabled) {
    config_.audio.captureSystemAudio = enabled;
}

void AppController::setMicrophone(bool enabled) {
    config_.audio.captureMicrophone = enabled;
}

void AppController::setSystemVolume(int volume) {
    config_.audio.systemVolume = volume;
}

void AppController::setMicVolume(int volume) {
    config_.audio.micVolume = volume;
}

void AppController::setDenoise(bool enabled) {
    config_.audio.denoise = enabled;
}

void AppController::setOutputDir(const QString& dir) {
    config_.general.outputDir = dir.toStdString();
}

void AppController::setFormat(const QString& format) {
    config_.general.format = format.toLower() == "mkv" ? "mkv" : "mp4";
}

void AppController::openOutputFolder() {
    const QString dir = QString::fromStdString(config_.general.outputDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void AppController::revealLastFile() {
    if (!lastFilePath_.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(lastFilePath_));
    }
}

} // namespace nr
