#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "ankiclient.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    AnkiClient client;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("anki"), &client);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("anki_app", "Main");

    return app.exec();
}
