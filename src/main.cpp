#include "ankiclient.h"
#include "common.h"
#include "penthread.h"
#include "qtfb-client.h"
#include "renderer.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <cstring>

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");

    {
        QByteArray binDir = QByteArray(argv[0]);
        int slash = binDir.lastIndexOf('/');
        binDir = (slash >= 0) ? binDir.left(slash) : QByteArray(".");
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", binDir + "/platforms");
    }

    QGuiApplication app(argc, argv);

    qtfb::ClientConnection qtfb(qtfb::getIDFromAppload(), FBFMT_RMPP_RGB888);
    qtfb.setRefreshMode(REFRESH_MODE_ANIMATE);

    const int screenW = static_cast<int>(qtfb.width());
    const int screenH = static_cast<int>(qtfb.height());

    PenThread penThread(&qtfb, screenW, screenH);

    QNetworkAccessManager nam;
    AnkiClient            anki;
    Renderer              renderer(&anki, &nam, &penThread, screenW, screenH);

    QObject::connect(&penThread, &PenThread::touchReceived,
                     &renderer,  &Renderer::handleTouch);

    QObject::connect(&penThread, &PenThread::terminateReceived,
                     []() { QCoreApplication::quit(); });

    QObject::connect(&renderer, &Renderer::frameReady, [&]() {
        const QImage &canvas = renderer.canvas();
        const QImage base = (canvas.format() == QImage::Format_RGB888)
                            ? canvas
                            : canvas.convertToFormat(QImage::Format_RGB888);

        if (!qtfb.shm) return;
        const size_t frameBytes = static_cast<size_t>(screenW) *
                                  static_cast<size_t>(screenH) * 3u;
        if (qtfb.shmSize < frameBytes) return;

        {
            std::lock_guard<std::mutex> shmLock(penThread.shmMutex());
            if (penThread.hasStrokes()) {
                QImage composed = base.copy();
                QPainter op(&composed);
                op.drawImage(0, 0, penThread.drawLayer());
                op.end();
                std::memcpy(qtfb.shm, composed.constBits(), frameBytes);
            } else {
                std::memcpy(qtfb.shm, base.constBits(), frameBytes);
            }
        }

        {
            std::lock_guard<std::mutex> writeLock(penThread.writeMutex());
            qtfb.setRefreshMode(REFRESH_MODE_CONTENT);
            qtfb.sendCompleteUpdate();
            qtfb.setRefreshMode(REFRESH_MODE_ANIMATE);
        }
    });

    penThread.start();

    return app.exec();
}
