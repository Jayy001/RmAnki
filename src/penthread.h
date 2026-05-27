#pragma once

#include "common.h"
#include "qtfb-client.h"

#include <QImage>
#include <QObject>
#include <QRect>

#include <atomic>
#include <mutex>
#include <thread>

class PenThread : public QObject
{
    Q_OBJECT

public:
    explicit PenThread(qtfb::ClientConnection *qtfb,
                       int screenW, int screenH,
                       QObject *parent = nullptr);
    ~PenThread();

    void start();
    void stop();

    std::mutex &shmMutex()   { return m_shmMutex;   }
    std::mutex &writeMutex() { return m_writeMutex; }

    bool hasStrokes() const { return m_hasStrokes.load(std::memory_order_relaxed); }

    // Caller must hold shmMutex().
    const QImage &drawLayer() const { return m_drawLayer; }
    void clearStrokes() {
        m_drawLayer.fill(Qt::transparent);
        m_hasStrokes.store(false, std::memory_order_relaxed);
    }

signals:
    void touchReceived(int x, int y, int inputType);
    void terminateReceived();
    void penFirstStroke();

private:
    void run();
    void drawSegment(int x0, int y0, int x1, int y1, int penW);
    inline void shmDisc(int cx, int cy, int r);

    qtfb::ClientConnection *m_qtfb;
    int  m_screenW, m_screenH;

    std::mutex m_shmMutex;
    std::mutex m_writeMutex;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};

    bool m_penDown = false;
    int  m_lastX   = 0;
    int  m_lastY   = 0;

    std::atomic<bool> m_hasStrokes{false};

    // Protected by m_shmMutex.
    QImage m_drawLayer;

    int m_curMode = -1;
};
