#include "penthread.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPainter>
#include <QPen>

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cstdlib>
#include <cmath>

// Pen evdev device
static constexpr const char *PEN_EVDEV = "/dev/input/event2";

// Evdev axis maxima (ABS_X 0–11180 → 1620px, ABS_Y 0–15340 → 2160px)
static constexpr float EVDEV_X_MAX = 11180.0f;
static constexpr float EVDEV_Y_MAX = 15340.0f;

PenThread::PenThread(qtfb::ClientConnection *qtfb,
                     int screenW, int screenH,
                     QObject *parent)
    : QObject(parent)
    , m_qtfb(qtfb)
    , m_screenW(screenW)
    , m_screenH(screenH)
    , m_drawLayer(screenW, screenH, QImage::Format_ARGB32_Premultiplied)
{
    m_drawLayer.fill(Qt::transparent);
}

PenThread::~PenThread()
{
    stop();
}

void PenThread::start()
{
    m_running.store(true, std::memory_order_relaxed);
    m_thread = std::thread(&PenThread::run, this);
}

void PenThread::stop()
{
    m_running.store(false, std::memory_order_relaxed);
    if (m_thread.joinable())
        m_thread.join();
}

void PenThread::run()
{
    const int qtfbFd = m_qtfb->socketFd();

    const int evFd = open(PEN_EVDEV, O_RDONLY | O_NONBLOCK);
    if (evFd < 0) {
        // Fall back: log and keep running (touch/terminate still work via QTFB)
        // In practice this shouldn't happen as root.
    }

    struct pollfd pfds[2];
    pfds[0].fd     = qtfbFd;
    pfds[0].events = POLLIN;
    pfds[1].fd     = evFd;    // may be -1 if open failed — poll ignores fd<0
    pfds[1].events = POLLIN;

    // Evdev accumulator — updated per EV_ABS event, consumed on SYN_REPORT
    int  evX        = 0;
    int  evY        = 0;
    int  evPressure = 0;

    const float scaleX = (float)m_screenW / EVDEV_X_MAX;
    const float scaleY = (float)m_screenH / EVDEV_Y_MAX;

    while (m_running.load(std::memory_order_relaxed)) {
        int ret = poll(pfds, (evFd >= 0) ? 2 : 1, /*timeout_ms=*/50);
        if (ret <= 0) continue;

        bool firstStrokePending = false;

        // Drain evdev — pen events
        if (evFd >= 0 && (pfds[1].revents & POLLIN)) {
            struct input_event ev;
            while (read(evFd, &ev, sizeof(ev)) == sizeof(ev)) {

                if (ev.type == EV_ABS) {
                    if      (ev.code == ABS_X)        evX        = ev.value;
                    else if (ev.code == ABS_Y)        evY        = ev.value;
                    else if (ev.code == ABS_PRESSURE) evPressure = ev.value;

                } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    if (ev.value) {
                        // pen press
                        bool firstStroke = !m_hasStrokes.load(std::memory_order_relaxed);
                        m_penDown = true;
                        int sx = (int)(evX * scaleX + 0.5f);
                        int sy = (int)(evY * scaleY + 0.5f);
                        sx = std::max(0, std::min(m_screenW - 1, sx));
                        sy = std::max(0, std::min(m_screenH - 1, sy));
                        m_lastX = sx;
                        m_lastY = sy;

                        int penW = std::max(2, evPressure * 6 / 4096 + 2);
                        {
                            std::lock_guard<std::mutex> lk(m_shmMutex);
                            m_hasStrokes.store(true, std::memory_order_relaxed);
                            drawSegment(sx, sy, sx, sy, penW);
                        }
                        {
                            int r = penW / 2 + 2;
                            std::lock_guard<std::mutex> lk(m_writeMutex);
                            if (m_curMode != REFRESH_MODE_ANIMATE) {
                                m_qtfb->setRefreshMode(REFRESH_MODE_ANIMATE);
                                m_curMode = REFRESH_MODE_ANIMATE;
                            }
                            m_qtfb->sendPartialUpdate(
                                std::max(0, sx - r), std::max(0, sy - r),
                                r * 2, r * 2);
                        }
                        if (firstStroke)
                            firstStrokePending = true;
                    } else {
                        // pen release
                        m_penDown = false;
                    }

                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    // end of event frame — draw segment
                    if (m_penDown) {
                        int sx = (int)(evX * scaleX + 0.5f);
                        int sy = (int)(evY * scaleY + 0.5f);
                        sx = std::max(0, std::min(m_screenW - 1, sx));
                        sy = std::max(0, std::min(m_screenH - 1, sy));

                        int penW = std::max(2, evPressure * 6 / 4096 + 2);
                        int x0 = m_lastX, y0 = m_lastY;
                        int x1 = sx,      y1 = sy;

                        int pad = penW / 2 + 2;
                        int rx  = std::min(x0, x1) - pad;
                        int ry  = std::min(y0, y1) - pad;
                        int rw  = std::abs(x1 - x0) + pad * 2;
                        int rh  = std::abs(y1 - y0) + pad * 2;
                        if (rx < 0) { rw += rx; rx = 0; }
                        if (ry < 0) { rh += ry; ry = 0; }
                        rw = std::min(rw, m_screenW - rx);
                        rh = std::min(rh, m_screenH - ry);

                        {
                            std::lock_guard<std::mutex> lk(m_shmMutex);
                            drawSegment(x0, y0, x1, y1, penW);
                        }
                        m_lastX = sx;
                        m_lastY = sy;

                        // Send immediately per SYN_REPORT frame
                        if (rw > 0 && rh > 0) {
                            std::lock_guard<std::mutex> lk(m_writeMutex);
                            if (m_curMode != REFRESH_MODE_ANIMATE) {
                                m_qtfb->setRefreshMode(REFRESH_MODE_ANIMATE);
                                m_curMode = REFRESH_MODE_ANIMATE;
                            }
                            m_qtfb->sendPartialUpdate(rx, ry, rw, rh);
                        }
                    }
                }
            } // while read
        } // evdev

        // Drain QTFB socket — touch + terminate only
        if (pfds[0].revents & POLLIN) {
            qtfb::ServerMessage msg;
            while (recv(qtfbFd, &msg, sizeof(msg), MSG_DONTWAIT) >= 1) {
                switch (msg.type) {
                case MESSAGE_USERINPUT: {
                    const auto &ui = msg.userInput;
                    switch (ui.inputType) {
                    case INPUT_TOUCH_PRESS:
                    case INPUT_TOUCH_RELEASE:
                    case INPUT_TOUCH_UPDATE: {
                        int x = ui.x, y = ui.y, t = ui.inputType;
                        QMetaObject::invokeMethod(this, [this, x, y, t]() {
                            emit touchReceived(x, y, t);
                        }, Qt::QueuedConnection);
                        break;
                    }
                    default: break; // pen events from QTFB shim — ignore
                    }
                    break;
                }
                case MESSAGE_TERMINATE:
                    QMetaObject::invokeMethod(this, [this]() {
                        emit terminateReceived();
                    }, Qt::QueuedConnection);
                    if (evFd >= 0) close(evFd);
                    return;
                default:
                    break;
                }
            }
        }

        if (firstStrokePending) {
            QMetaObject::invokeMethod(this, [this]() {
                emit penFirstStroke();
            }, Qt::QueuedConnection);
        }
    }

    if (evFd >= 0) close(evFd);
}

// drawSegment — Bresenham thick line to shm and m_drawLayer. Caller holds m_shmMutex.
void PenThread::drawSegment(int x0, int y0, int x1, int y1, int penW)
{
    // QPainter path — antialiased, used during full renders
    {
        QPainter dp(&m_drawLayer);
        dp.setRenderHint(QPainter::Antialiasing);
        dp.setPen(QPen(Qt::black, penW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        dp.drawLine(x0, y0, x1, y1);
    }

    // Direct Bresenham to shm (RGB888)
    int r = penW / 2;
    int dx =  std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        shmDisc(x0, y0, r);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// shmDisc — write a filled disc of black pixels to shm at (cx,cy) radius r
inline void PenThread::shmDisc(int cx, int cy, int r)
{
    const int stride = m_screenW * 3;
    for (int dy = -r; dy <= r; ++dy) {
        int y = cy + dy;
        if (y < 0 || y >= m_screenH) continue;
        int limit = (int)std::sqrt((double)(r * r - dy * dy));
        int xStart = cx - limit;
        int xEnd   = cx + limit;
        if (xStart < 0)          xStart = 0;
        if (xEnd   >= m_screenW) xEnd   = m_screenW - 1;
        if (xStart > xEnd) continue;
        uint8_t *row = m_qtfb->shm + y * stride + xStart * 3;
        std::memset(row, 0, (xEnd - xStart + 1) * 3);
    }
}
