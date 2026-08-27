#include <QGuiApplication>
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "ui/AppController.h"

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("NotionRecorder");
    QCoreApplication::setOrganizationName("NotionRecorder");

    nr::AppController controller;
    QQmlApplicationEngine engine;
    engine.addImportPath(QCoreApplication::applicationDirPath() +
                         QStringLiteral("/../Resources/qml"));
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
