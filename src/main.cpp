#include <QGuiApplication>
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/RecordingSession.h"
#if defined(__APPLE__)
#include "capture/macos/CoreAudioTapCapturer.h"
#endif
#include "ui/AppController.h"

namespace {

// Hidden diagnostic mode: records the full screen (with system audio) for the
// given number of seconds, then prints audio-pipeline counters to stderr.
// Runs under the same app bundle so ScreenCaptureKit permissions apply.
int runRecordTest(int seconds) {
    if (seconds <= 0) {
        seconds = 8;
    }
    if (seconds > 120) {
        seconds = 120;
    }
    nr::RecordingConfig config = nr::RecordingConfig::defaults();
    config.general.outputDir = "/tmp/nr_audio_test";
    config.video.fps = 30;
    config.video.width = 1280;
    config.video.height = 720;
    config.audio.captureSystemAudio = true;
    config.audio.captureMicrophone = false;
    config.audio.denoise = false;

    fprintf(stderr, "[record-test] starting %ds recording...\n", seconds);
    nr::RecordingSession session;
    if (!session.start(config, [](nr::RecordingSession::State) {})) {
        fprintf(stderr, "[record-test] start failed: %s\n",
                session.lastError().c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    session.stop();

    const auto s = session.audioDebugStats();
    fprintf(stderr,
            "[record-test] output=%s\n"
            "[record-test] sckCallbacks=%lld sckEmptyDrops=%lld sckSamples=%lld\n"
            "[record-test] sckAudioError=\"%s\" sckParseReason=\"%s\" "
            "(noFormat=%lld noASBD=%lld noBufferList=%lld bufferError=%lld "
            "noChannels=%lld noSamples=%lld)\n"
            "[record-test] systemFrames=%lld tapFrames=%lld tapActive=%d tapSetupErrors=%lld\n"
            "[record-test] mixPushed=%lld mixDropped=%lld\n"
            "[record-test] encodedFrames=%lld encodeFailures=%lld\n"
            "[record-test] encodedPackets=%lld muxedPackets=%lld muxFailures=%lld\n",
            session.outputPath().c_str(),
            s.sckAudioCallbacks, s.sckAudioEmptyDrops, s.sckAudioSamples,
            s.sckAudioError.c_str(), s.sckParseReason.c_str(),
            s.parseNoFormat, s.parseNoASBD, s.parseNoBufferList,
            s.parseBufferError, s.parseNoChannels, s.parseNoSamples,
            s.systemAudioFrames, s.tapFrames, s.tapActive ? 1 : 0, s.tapSetupErrors,
            s.mixPushed, s.mixDropped,
            s.encodedFrames, s.encodeFailures,
            s.encodedPackets, s.muxedPackets, s.muxFailures);
    return 0;
}

// Hidden diagnostic mode: starts only the CoreAudio system-audio tap (no
// screen capture needed) and reports whether audio buffers arrive.
#if defined(__APPLE__)
int runTapTest(int seconds) {
    if (seconds <= 0) {
        seconds = 6;
    }
    nr::AudioConfig audio;
    audio.captureSystemAudio = true;
    audio.systemVolume = 100;
    audio.sampleRate = 48000;
    audio.channels = 2;

    std::atomic<long long> frames{0};
    std::atomic<long long> samples{0};
    std::atomic<long long> callbacks{0};
    std::vector<float> allSamples;
    std::mutex samplesMutex;
    nr::CoreAudioTapCapturer capturer;
    if (!capturer.start(audio, [&](nr::AudioFrame f) {
            frames.fetch_add(1);
            samples.fetch_add(static_cast<long long>(f.samples.size()));
            callbacks.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(samplesMutex);
                allSamples.insert(allSamples.end(), f.samples.begin(),
                                  f.samples.end());
            }
        })) {
        const auto s = capturer.stats();
        fprintf(stderr, "[tap-test] 启动失败 setupErrors=%lld\n", s.setupErrors);
        return 1;
    }
    fprintf(stderr, "[tap-test] 已启动，等待 %ds...\n", seconds);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    capturer.stop();
    fprintf(stderr, "[tap-test] callbacks=%lld frames=%lld samples=%lld "
                    "(~%.1f callbacks/s, ~%.1f kSamples/s)\n",
            callbacks.load(), frames.load(), samples.load(),
            static_cast<double>(callbacks.load()) / seconds,
            static_cast<double>(samples.load()) / 1000.0 / seconds);
    {
        std::lock_guard<std::mutex> lock(samplesMutex);
        FILE* f = std::fopen("/tmp/nr_tap.raw", "wb");
        if (f) {
            std::fwrite(allSamples.data(), sizeof(float), allSamples.size(), f);
            std::fclose(f);
            fprintf(stderr, "[tap-test] 已保存 /tmp/nr_tap.raw (%zu samples)\n",
                    allSamples.size());
        }
    }
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--record-test") {
        return runRecordTest(std::atoi(argv[2]));
    }
#if defined(__APPLE__)
    if (argc >= 3 && std::string(argv[1]) == "--tap-test") {
        return runTapTest(std::atoi(argv[2]));
    }
#endif
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("NotionRecorder");
    QCoreApplication::setOrganizationName("NotionRecorder");

    nr::AppController controller;
    QQmlApplicationEngine engine;
    // On macOS the QML module lives inside the .app bundle's Resources
    // directory; on Windows it is next to the executable.
#if defined(__APPLE__)
    engine.addImportPath(QCoreApplication::applicationDirPath() +
                         QStringLiteral("/../Resources/qml"));
#else
    engine.addImportPath(QCoreApplication::applicationDirPath() +
                         QStringLiteral("/qml"));
#endif
    engine.rootContext()->setContextProperty("app", &controller);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [&app](const QUrl& url) {
            qWarning("Failed to load QML: %s", qPrintable(url.toString()));
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);

    engine.loadFromModule("NotionRecorder", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    return app.exec();
}
