#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include "core/Config.h"
#include "core/RecordingSession.h"

namespace nr {

// Bridges the recording core to QML. Owns the session and exposes the current
// configuration as invokables so the Notion-style UI stays declarative.
class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool recording READ isRecording NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ isPaused NOTIFY stateChanged)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY elapsedChanged)
    Q_PROPERTY(QString encoderName READ encoderName NOTIFY stateChanged)
    Q_PROPERTY(int inputQueueDepth READ inputQueueDepth NOTIFY statsChanged)
    Q_PROPERTY(int outputQueueDepth READ outputQueueDepth NOTIFY statsChanged)
    Q_PROPERTY(int cpuPercent READ cpuPercent NOTIFY statsChanged)
    Q_PROPERTY(bool highLoad READ highLoad NOTIFY statsChanged)
    Q_PROPERTY(QString lastFilePath READ lastFilePath NOTIFY sessionFinished)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    QString statusText() const;
    bool isRecording() const;
    bool isPaused() const;
    QString elapsedText() const;
    QString encoderName() const;
    int inputQueueDepth() const;
    int outputQueueDepth() const;
    int cpuPercent() const;
    bool highLoad() const;
    QString lastFilePath() const;
    QString errorMessage() const;

    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void setRegion(int x, int y, int width, int height);
    Q_INVOKABLE void setCaptureMode(int mode);  // 0 = full screen, 1 = region
    Q_INVOKABLE void setFps(int fps);           // 0 = auto
    Q_INVOKABLE void setCaptureCursor(bool enabled);
    Q_INVOKABLE void setClickEffects(bool enabled);
    Q_INVOKABLE void setCodec(const QString& codec);
    Q_INVOKABLE void setBitrateMode(int mode);  // 0 = quality, 1 = file size
    Q_INVOKABLE void setBitrateKbps(int kbps);
    Q_INVOKABLE void setCrf(int crf);
    Q_INVOKABLE void setSystemAudio(bool enabled);
    Q_INVOKABLE void setMicrophone(bool enabled);
    Q_INVOKABLE void setSystemVolume(int volume);
    Q_INVOKABLE void setMicVolume(int volume);
    Q_INVOKABLE void setDenoise(bool enabled);
    Q_INVOKABLE void setOutputDir(const QString& dir);
    Q_INVOKABLE void setFormat(const QString& format);
    Q_INVOKABLE void openOutputFolder();
    Q_INVOKABLE void revealLastFile();

Q_SIGNALS:
    void stateChanged();
    void elapsedChanged();
    void statsChanged();
    void sessionFinished(const QString& path);
    void recordingFailed(const QString& message);

private:
    void handleState(RecordingSession::State state);
    void startRecording();
    void stopRecording();
    void refreshStats();
    void setupHotkeys();

    RecordingConfig config_;
    std::unique_ptr<RecordingSession> session_;
    QTimer statsTimer_;
    QString lastFilePath_;
    QString errorMessage_;

    // Global hotkeys (Carbon EventHotKey): ⌘⇧R start/stop, ⌘⇧P pause.
    void* hotKeyEventHandlerRef_ = nullptr;
    void* hotKeyToggleRef_ = nullptr;
    void* hotKeyPauseRef_ = nullptr;

    // CPU sampling state.
    long long lastUserTicks_ = 0;
    long long lastSystemTicks_ = 0;
    long long lastIdleTicks_ = 0;
    int cpuPercent_ = 0;
};

} // namespace nr
