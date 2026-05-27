#pragma once

#include "ankiclient.h"
#include "htmlview.h"
#include "penthread.h"

#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QTimer>
#include <QVector>

class Renderer : public QObject
{
    Q_OBJECT

public:
    explicit Renderer(AnkiClient *anki,
                      QNetworkAccessManager *nam,
                      PenThread *penThread,
                      int width, int height,
                      QObject *parent = nullptr);
    ~Renderer() = default;

    void handleTouch(int x, int y, int inputType);

    const QImage &canvas() const { return m_canvas; }

signals:
    void frameReady();

private slots:
    void scheduleRender();

private:
    void render();
    void drawHeader(QPainter &p);
    void drawFooter(QPainter &p);
    void drawLoadingView(QPainter &p, const QRect &area);
    void drawErrorView(QPainter &p, const QRect &area);
    void drawDecksView(QPainter &p, const QRect &area);
    void drawDoneView(QPainter &p, const QRect &area);
    void drawStudyView(QPainter &p, const QRect &area);
    void drawLoginView(QPainter &p);
    void drawButton(QPainter &p, const QRect &r, const QString &label,
                    const QColor &border, const QColor &bg, const QColor &fg,
                    int radius = 24, int borderWidth = 3, int fontSize = -1);

    static constexpr int W      = 1620;
    static constexpr int H      = 2160;
    static constexpr int HDR_H  = 140;
    static constexpr int FTR_H  = 80;
    static constexpr int BTN_H  = 300;
    static constexpr int MARGIN = 60;

    static constexpr int FS_HEADER = 48;
    static constexpr int FS_LARGE  = 72;
    static constexpr int FS_NORMAL = 52;
    static constexpr int FS_SMALL  = 40;
    static constexpr int FS_KEY    = 48;

    AnkiClient *m_anki;
    PenThread  *m_penThread;
    QImage      m_canvas;
    HtmlView   *m_frontView;
    HtmlView   *m_backView;
    QTimer      m_renderTimer;

    bool m_answerRevealed  = false;
    int  m_deckScrollOffset = 0;
    QRect m_eraserRect;
    QPoint m_touchDown;

    int     m_kbFocusedField = 0;
    QString m_kbEmail;
    QString m_kbPassword;
    bool    m_kbShifted = false;
    bool    m_kbSymbols = false;

    struct HitRect { QRect rect; std::function<void()> action; };
    QVector<HitRect> m_hitRects;
    void addHit(const QRect &r, std::function<void()> fn) {
        m_hitRects.push_back({r, std::move(fn)});
    }
};
