#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <QDir>

namespace {

class StubApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool annotationMode READ annotationMode CONSTANT)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps CONSTANT)
    Q_PROPERTY(int bitrateMode READ bitrateMode CONSTANT)
    Q_PROPERTY(bool captureCursor READ captureCursor CONSTANT)
    Q_PROPERTY(int captureModeIndex READ captureModeIndex CONSTANT)
    Q_PROPERTY(bool clickEffects READ clickEffects CONSTANT)
    Q_PROPERTY(QString codec READ codec CONSTANT)
    Q_PROPERTY(int cpuPercent READ cpuPercent CONSTANT)
    Q_PROPERTY(int crf READ crf CONSTANT)
    Q_PROPERTY(bool denoise READ denoise CONSTANT)
    Q_PROPERTY(QString encoderName READ encoderName CONSTANT)
    Q_PROPERTY(int formatIndex READ formatIndex CONSTANT)
    Q_PROPERTY(int fps READ fps CONSTANT)
    Q_PROPERTY(bool highLoad READ highLoad CONSTANT)
    Q_PROPERTY(int micVolume READ micVolume CONSTANT)
    Q_PROPERTY(bool microphone READ microphone CONSTANT)
    Q_PROPERTY(QString outputDir READ outputDir CONSTANT)
    Q_PROPERTY(bool paused READ paused CONSTANT)
    Q_PROPERTY(bool recording READ recording CONSTANT)
    Q_PROPERTY(QVariantList recordingHistory READ recordingHistory CONSTANT)
    Q_PROPERTY(int scheduledDelay READ scheduledDelay CONSTANT)
    Q_PROPERTY(int scheduledDuration READ scheduledDuration CONSTANT)
    Q_PROPERTY(bool scheduledRecording READ scheduledRecording CONSTANT)
    Q_PROPERTY(QString statusText READ statusText CONSTANT)
    Q_PROPERTY(bool systemAudio READ systemAudio CONSTANT)
    Q_PROPERTY(int systemVolume READ systemVolume CONSTANT)
    Q_PROPERTY(QVariantList windowList READ windowList CONSTANT)

public:
    bool annotationMode() const { return false; }
    int bitrateKbps() const { return 6000; }
    int bitrateMode() const { return 1; }
    bool captureCursor() const { return true; }
    int captureModeIndex() const { return 0; }
    bool clickEffects() const { return false; }
    QString codec() const { return QStringLiteral("auto"); }
    int cpuPercent() const { return 0; }
    int crf() const { return 18; }
    bool denoise() const { return false; }
    QString encoderName() const { return QString(); }
    int formatIndex() const { return 0; }
    int fps() const { return 60; }
    bool highLoad() const { return false; }
    int micVolume() const { return 100; }
    bool microphone() const { return false; }
    QString outputDir() const { return QStringLiteral("~/Movies/NotionRecorder"); }
    bool paused() const { return false; }
    bool recording() const { return false; }
    QVariantList recordingHistory() const { return {}; }
    int scheduledDelay() const { return 0; }
    int scheduledDuration() const { return 0; }
    bool scheduledRecording() const { return false; }
    QString statusText() const { return QStringLiteral("准备就绪"); }
    bool systemAudio() const { return true; }
    int systemVolume() const { return 100; }
    QVariantList windowList() const { return {}; }

    Q_INVOKABLE void openOutputFolder() {}
    Q_INVOKABLE void pickWindow(int) {}
    Q_INVOKABLE void refreshHistory() {}
    Q_INVOKABLE void refreshWindows() {}
    Q_INVOKABLE void revealRecording(const QString&) {}
    Q_INVOKABLE void setBitrateKbps(int) {}
    Q_INVOKABLE void setBitrateMode(int) {}
    Q_INVOKABLE void setCaptureCursor(bool) {}
    Q_INVOKABLE void setCaptureMode(int) {}
    Q_INVOKABLE void setClickEffects(bool) {}
    Q_INVOKABLE void setCodec(const QString&) {}
    Q_INVOKABLE void setCrf(int) {}
    Q_INVOKABLE void setDenoise(bool) {}
    Q_INVOKABLE void setFormat(const QString&) {}
    Q_INVOKABLE void setFps(int) {}
    Q_INVOKABLE void setMicVolume(int) {}
    Q_INVOKABLE void setMicrophone(bool) {}
    Q_INVOKABLE void setOutputDir(const QString&) {}
    Q_INVOKABLE void setRegion(int, int, int, int) {}
    Q_INVOKABLE void setScheduledDelay(int) {}
    Q_INVOKABLE void setScheduledDuration(int) {}
    Q_INVOKABLE void setScheduledRecording(bool) {}
    Q_INVOKABLE void setSystemAudio(bool) {}
    Q_INVOKABLE void setSystemVolume(int) {}
    Q_INVOKABLE void toggleAnnotationMode() {}
    Q_INVOKABLE void toggleRecording() { ++toggleCalls; }
    Q_INVOKABLE void togglePause() { ++pauseCalls; }

    int toggleCalls = 0;
    int pauseCalls = 0;

Q_SIGNALS:
    void settingsChanged();
    void annotationModeChanged();
    void sessionFinished(const QString& path);
    void recordingFailed(const QString& message);
};

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // Load the real Main.qml and verify that clicking the floating-bar
    // icons opens the corresponding windows.
    QQmlApplicationEngine mainEngine;
    mainEngine.addImportPath(QStringLiteral("/opt/homebrew/opt/qt/qml"));
    mainEngine.addImportPath(QStringLiteral("."));
    mainEngine.addImportPath(QCoreApplication::applicationDirPath());
    StubApp mainStub;
    mainEngine.rootContext()->setContextProperty(QStringLiteral("app"), &mainStub);

    mainEngine.loadFromModule(QStringLiteral("NotionRecorder"), QStringLiteral("Main"));
    qInfo("app context valid after load: %d",
          mainEngine.rootContext()->contextProperty(QStringLiteral("app")).isValid());
    if (mainEngine.rootObjects().isEmpty()) {
        qWarning("Main.qml failed to load from module");
        return 3;
    }
    auto* mainWindow = qobject_cast<QQuickWindow*>(mainEngine.rootObjects().first());
    if (!mainWindow) {
        qWarning("Main root is not a window");
        return 3;
    }
    mainWindow->show();
    QTest::qWait(200);

    struct IconCheck {
        const char* button;
        const char* dialog;
        const char* label;
    };
    const IconCheck checks[] = {
        {"settingsButton", "settingsDialogWindow", "settings"},
        {"regionButton", "regionSelectorWindow", "region"},
        {"filesButton", "historyDialogWindow", "history"},
    };

    for (const IconCheck& check : checks) {
        QQuickItem* button =
            mainWindow->findChild<QQuickItem*>(QString::fromLatin1(check.button));
        if (!button) {
            qWarning("%s button not found", check.label);
            return 3;
        }
        const QPoint scene = button->mapToScene(QPointF(15, 15)).toPoint();
        QTest::mouseClick(mainWindow, Qt::LeftButton, Qt::NoModifier, scene);
        QTest::qWait(200);

        QObject* dialog =
            mainWindow->findChild<QObject*>(QString::fromLatin1(check.dialog));
        if (!dialog) {
            qWarning("%s dialog object not found", check.label);
            return 3;
        }
        const bool visible = dialog->property("visible").toBool();
        qInfo("%s window visible after click = %d", check.label, visible);
        if (!visible) {
            return 4;
        }
        // Close it before the next check.
        dialog->setProperty("visible", false);
        QTest::qWait(80);
    }
    return 0;
}

#include "qml_click_test.moc"
