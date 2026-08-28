#include "ui/AppController.h"

#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>
#include <QVariantMap>

#include <cmath>

#include "capture/macos/ScreenCaptureKitCapturer.h"

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
    loadSettings();
    connect(&statsTimer_, &QTimer::timeout, this, &AppController::refreshStats);
    statsTimer_.setInterval(250);
    connect(&scheduleTimer_, &QTimer::timeout, this, [this] {
        if (countdownLeft_ > 0) {
            --countdownLeft_;
            Q_EMIT scheduleChanged();
            Q_EMIT stateChanged();  // countdown shown in the status text
            return;
        }
        scheduleTimer_.stop();
        startRecording();
        if (config_.general.scheduledDurationSec > 0 && session_) {
            QTimer::singleShot(config_.general.scheduledDurationSec * 1000, this, [this] {
                if (session_ && (isRecording() || isPaused())) {
                    stopRecording();
                }
            });
        }
    });
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
    if (isScheduling()) {
        return "将在 " + QString::number(countdownLeft_) + " 秒后开始录制";
    }
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

QVariantList AppController::windowList() const {
    return windowList_;
}

QString AppController::lastFilePath() const {
    return lastFilePath_;
}

QString AppController::errorMessage() const {
    return errorMessage_;
}

int AppController::captureModeIndex() const {
    switch (config_.video.mode) {
        case CaptureMode::Region:
            return 1;
        case CaptureMode::Window:
            return 2;
        default:
            return 0;
    }
}

int AppController::fps() const {
    return config_.video.fps;
}

bool AppController::captureCursor() const {
    return config_.video.captureCursor;
}

bool AppController::clickEffects() const {
    return config_.video.clickEffects;
}

QString AppController::codec() const {
    return QString::fromStdString(config_.video.codec);
}

int AppController::bitrateMode() const {
    return config_.video.bitrateMode == BitrateMode::Quality ? 0 : 1;
}

int AppController::bitrateKbps() const {
    return config_.video.bitrateKbps;
}

int AppController::crf() const {
    return config_.video.crf;
}

bool AppController::systemAudio() const {
    return config_.audio.captureSystemAudio;
}

bool AppController::microphone() const {
    return config_.audio.captureMicrophone;
}

int AppController::systemVolume() const {
    return config_.audio.systemVolume;
}

int AppController::micVolume() const {
    return config_.audio.micVolume;
}

bool AppController::denoise() const {
    return config_.audio.denoise;
}

bool AppController::annotationMode() const {
    return config_.video.annotationMode;
}

QString AppController::outputDir() const {
    return QString::fromStdString(config_.general.outputDir);
}

int AppController::formatIndex() const {
    return config_.general.format == "mkv" ? 1 : 0;
}

void AppController::toggleRecording() {
    if (isScheduling()) {
        cancelSchedule();
        Q_EMIT stateChanged();
        return;
    }
    if (session_ && (session_->state() == RecordingSession::State::Recording ||
                     session_->state() == RecordingSession::State::Paused)) {
        stopRecording();
    } else if (!session_ || session_->state() == RecordingSession::State::Idle) {
        if (config_.general.scheduledRecording && config_.general.scheduledDelaySec > 0) {
            startCountdown();
        } else {
            startRecording();
        }
    }
}

bool AppController::isScheduling() const {
    return scheduleTimer_.isActive() && countdownLeft_ > 0;
}

void AppController::startCountdown() {
    if (session_ && session_->state() != RecordingSession::State::Idle) {
        return;
    }
    countdownLeft_ = config_.general.scheduledDelaySec;
    scheduleTimer_.start();
    Q_EMIT scheduleChanged();
    Q_EMIT stateChanged();
}

void AppController::cancelSchedule() {
    scheduleTimer_.stop();
    countdownLeft_ = 0;
    Q_EMIT scheduleChanged();
}

bool AppController::scheduledRecording() const {
    return config_.general.scheduledRecording;
}

void AppController::setScheduledRecording(bool enabled) {
    if (config_.general.scheduledRecording == enabled) {
        return;
    }
    config_.general.scheduledRecording = enabled;
    saveSettings();
}

int AppController::scheduledDelay() const {
    return config_.general.scheduledDelaySec;
}

void AppController::setScheduledDelay(int seconds) {
    seconds = qBound(0, seconds, 3600);
    if (config_.general.scheduledDelaySec == seconds) {
        return;
    }
    config_.general.scheduledDelaySec = seconds;
    saveSettings();
}

int AppController::scheduledDuration() const {
    return config_.general.scheduledDurationSec;
}

void AppController::setScheduledDuration(int seconds) {
    seconds = qBound(0, seconds, 3600);
    if (config_.general.scheduledDurationSec == seconds) {
        return;
    }
    config_.general.scheduledDurationSec = seconds;
    saveSettings();
}

QVariantList AppController::recordingHistory() const {
    return recordingHistory_;
}

void AppController::refreshHistory() {
    recordingHistory_.clear();
    QString dir = QString::fromStdString(config_.general.outputDir);
    if (dir.startsWith(QLatin1Char('~'))) {
        dir.replace(0, 1, QDir::homePath());
    }
    QDir directory(dir);
    const QFileInfoList files =
        directory.entryInfoList({QStringLiteral("*.mp4"), QStringLiteral("*.mkv")},
                                QDir::Files, QDir::Time);
    for (const QFileInfo& file : files) {
        QVariantMap entry;
        entry.insert(QStringLiteral("path"), file.absoluteFilePath());
        entry.insert(QStringLiteral("name"), file.fileName());
        entry.insert(QStringLiteral("size"), file.size());
        entry.insert(QStringLiteral("modified"),
                     file.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        recordingHistory_.append(entry);
        if (recordingHistory_.size() >= 200) {
            break;
        }
    }
    Q_EMIT historyChanged();
}

void AppController::revealRecording(const QString& path) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
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
            refreshHistory();
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
    saveSettings();
}

void AppController::setCaptureMode(int mode) {
    if (mode == 1) {
        config_.video.mode = CaptureMode::Region;
    } else if (mode == 2) {
        config_.video.mode = CaptureMode::Window;
    } else {
        config_.video.mode = CaptureMode::FullScreen;
    }
    if (mode != 2) {
        config_.video.windowId = 0;
    }
    saveSettings();
}

void AppController::refreshWindows() {
    windowInfos_ = ScreenCaptureKitCapturer::listWindows();
    windowList_.clear();
    for (const WindowInfo& w : windowInfos_) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), w.id);
        entry.insert(QStringLiteral("title"), QString::fromStdString(w.title));
        entry.insert(QStringLiteral("application"), QString::fromStdString(w.application));
        entry.insert(QStringLiteral("width"), w.width);
        entry.insert(QStringLiteral("height"), w.height);
        windowList_.append(entry);
    }
    Q_EMIT windowsChanged();
}

void AppController::pickWindow(int windowId) {
    for (const WindowInfo& w : windowInfos_) {
        if (w.id == windowId) {
            config_.video.mode = CaptureMode::Window;
            config_.video.windowId = windowId;
            config_.video.width = w.width;
            config_.video.height = w.height;
            config_.video.region = Region{};
            saveSettings();
            Q_EMIT windowsChanged();
            return;
        }
    }
}

void AppController::setFps(int fps) {
    config_.video.fps = fps;
    saveSettings();
}

void AppController::setCaptureCursor(bool enabled) {
    config_.video.captureCursor = enabled;
    saveSettings();
}

void AppController::setClickEffects(bool enabled) {
    config_.video.clickEffects = enabled;
    saveSettings();
}

void AppController::setCodec(const QString& codec) {
    config_.video.codec = codec.toStdString();
    saveSettings();
}

void AppController::setBitrateMode(int mode) {
    config_.video.bitrateMode = mode == 0 ? BitrateMode::Quality : BitrateMode::FileSize;
    saveSettings();
}

void AppController::setBitrateKbps(int kbps) {
    config_.video.bitrateKbps = kbps;
    saveSettings();
}

void AppController::setCrf(int crf) {
    config_.video.crf = crf;
    saveSettings();
}

void AppController::setSystemAudio(bool enabled) {
    config_.audio.captureSystemAudio = enabled;
    saveSettings();
}

void AppController::setMicrophone(bool enabled) {
    config_.audio.captureMicrophone = enabled;
    saveSettings();
}

void AppController::setSystemVolume(int volume) {
    config_.audio.systemVolume = volume;
    saveSettings();
}

void AppController::setMicVolume(int volume) {
    config_.audio.micVolume = volume;
    saveSettings();
}

void AppController::setDenoise(bool enabled) {
    config_.audio.denoise = enabled;
    saveSettings();
}

void AppController::toggleAnnotationMode() {
    config_.video.annotationMode = !config_.video.annotationMode;
    saveSettings();
    Q_EMIT annotationModeChanged();
}

void AppController::setOutputDir(const QString& dir) {
    config_.general.outputDir = dir.toStdString();
    saveSettings();
}

void AppController::setFormat(const QString& format) {
    config_.general.format = format.toLower() == "mkv" ? "mkv" : "mp4";
    saveSettings();
}

void AppController::loadSettings() {
    QSettings s(QStringLiteral("NotionRecorder"), QStringLiteral("NotionRecorder"));
    config_.general.outputDir =
        s.value(QStringLiteral("general/outputDir"),
                QString::fromStdString(config_.general.outputDir))
            .toString()
            .toStdString();
    config_.general.format =
        s.value(QStringLiteral("general/format"),
                QString::fromStdString(config_.general.format))
            .toString()
            .toStdString();

    config_.video.mode =
        static_cast<CaptureMode>(s.value(QStringLiteral("video/mode"),
                                         static_cast<int>(config_.video.mode))
                                     .toInt());
    config_.video.windowId =
        s.value(QStringLiteral("video/windowId"), config_.video.windowId).toInt();
    config_.video.region.x =
        s.value(QStringLiteral("video/regionX"), config_.video.region.x).toInt();
    config_.video.region.y =
        s.value(QStringLiteral("video/regionY"), config_.video.region.y).toInt();
    config_.video.region.width =
        s.value(QStringLiteral("video/regionWidth"), config_.video.region.width).toInt();
    config_.video.region.height =
        s.value(QStringLiteral("video/regionHeight"), config_.video.region.height).toInt();
    config_.video.fps =
        s.value(QStringLiteral("video/fps"), config_.video.fps).toInt();
    config_.video.captureCursor =
        s.value(QStringLiteral("video/captureCursor"), config_.video.captureCursor).toBool();
    config_.video.clickEffects =
        s.value(QStringLiteral("video/clickEffects"), config_.video.clickEffects).toBool();
    config_.video.codec =
        s.value(QStringLiteral("video/codec"),
                QString::fromStdString(config_.video.codec))
            .toString()
            .toStdString();
    config_.video.bitrateMode =
        static_cast<BitrateMode>(s.value(QStringLiteral("video/bitrateMode"),
                                         static_cast<int>(config_.video.bitrateMode))
                                     .toInt());
    config_.video.bitrateKbps =
        s.value(QStringLiteral("video/bitrateKbps"), config_.video.bitrateKbps).toInt();
    config_.video.crf = s.value(QStringLiteral("video/crf"), config_.video.crf).toInt();
    config_.video.annotationMode =
        s.value(QStringLiteral("video/annotationMode"), config_.video.annotationMode)
            .toBool();

    config_.audio.captureSystemAudio =
        s.value(QStringLiteral("audio/captureSystemAudio"),
                config_.audio.captureSystemAudio)
            .toBool();
    config_.audio.captureMicrophone =
        s.value(QStringLiteral("audio/captureMicrophone"),
                config_.audio.captureMicrophone)
            .toBool();
    config_.audio.systemVolume =
        s.value(QStringLiteral("audio/systemVolume"), config_.audio.systemVolume).toInt();
    config_.audio.micVolume =
        s.value(QStringLiteral("audio/micVolume"), config_.audio.micVolume).toInt();
    config_.audio.denoise =
        s.value(QStringLiteral("audio/denoise"), config_.audio.denoise).toBool();
    config_.general.scheduledRecording =
        s.value(QStringLiteral("general/scheduledRecording"),
                config_.general.scheduledRecording)
            .toBool();
    config_.general.scheduledDelaySec =
        s.value(QStringLiteral("general/scheduledDelaySec"),
                config_.general.scheduledDelaySec)
            .toInt();
    config_.general.scheduledDurationSec =
        s.value(QStringLiteral("general/scheduledDurationSec"),
                config_.general.scheduledDurationSec)
            .toInt();
}

void AppController::saveSettings() {
    QSettings s(QStringLiteral("NotionRecorder"), QStringLiteral("NotionRecorder"));
    s.setValue(QStringLiteral("general/outputDir"),
               QString::fromStdString(config_.general.outputDir));
    s.setValue(QStringLiteral("general/format"),
               QString::fromStdString(config_.general.format));

    s.setValue(QStringLiteral("video/mode"), static_cast<int>(config_.video.mode));
    s.setValue(QStringLiteral("video/windowId"), config_.video.windowId);
    s.setValue(QStringLiteral("video/regionX"), config_.video.region.x);
    s.setValue(QStringLiteral("video/regionY"), config_.video.region.y);
    s.setValue(QStringLiteral("video/regionWidth"), config_.video.region.width);
    s.setValue(QStringLiteral("video/regionHeight"), config_.video.region.height);
    s.setValue(QStringLiteral("video/fps"), config_.video.fps);
    s.setValue(QStringLiteral("video/captureCursor"), config_.video.captureCursor);
    s.setValue(QStringLiteral("video/clickEffects"), config_.video.clickEffects);
    s.setValue(QStringLiteral("video/codec"),
               QString::fromStdString(config_.video.codec));
    s.setValue(QStringLiteral("video/bitrateMode"),
               static_cast<int>(config_.video.bitrateMode));
    s.setValue(QStringLiteral("video/bitrateKbps"), config_.video.bitrateKbps);
    s.setValue(QStringLiteral("video/crf"), config_.video.crf);
    s.setValue(QStringLiteral("video/annotationMode"), config_.video.annotationMode);

    s.setValue(QStringLiteral("audio/captureSystemAudio"),
               config_.audio.captureSystemAudio);
    s.setValue(QStringLiteral("audio/captureMicrophone"),
               config_.audio.captureMicrophone);
    s.setValue(QStringLiteral("audio/systemVolume"), config_.audio.systemVolume);
    s.setValue(QStringLiteral("audio/micVolume"), config_.audio.micVolume);
    s.setValue(QStringLiteral("audio/denoise"), config_.audio.denoise);
    s.setValue(QStringLiteral("general/scheduledRecording"),
               config_.general.scheduledRecording);
    s.setValue(QStringLiteral("general/scheduledDelaySec"),
               config_.general.scheduledDelaySec);
    s.setValue(QStringLiteral("general/scheduledDurationSec"),
               config_.general.scheduledDurationSec);
    Q_EMIT settingsChanged();
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
