#include "renderer.h"
#include "common.h"

#include <QDateTime>
#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <functional>

Renderer::Renderer(AnkiClient *anki,
                   QNetworkAccessManager *nam,
                   PenThread *penThread,
                   int width, int height,
                   QObject *parent)
    : QObject(parent)
    , m_anki(anki)
    , m_penThread(penThread)
    , m_canvas(width, height, QImage::Format_RGB888)
    , m_frontView(new HtmlView(nam, this))
    , m_backView(new HtmlView(nam, this))
{
    m_canvas.fill(Qt::white);

    // Connect AnkiClient signals → schedule re-render
    connect(m_anki, &AnkiClient::currentStateChanged,    this, &Renderer::scheduleRender);
    connect(m_anki, &AnkiClient::deckDataChanged,        this, &Renderer::scheduleRender);
    connect(m_anki, &AnkiClient::currentFrontChanged,    this, [this] {
        m_answerRevealed = false;
        // Clear pen strokes on new card
        {
            std::lock_guard<std::mutex> lock(m_penThread->shmMutex());
            m_penThread->clearStrokes();
        }
        m_frontView->setHtml(m_anki->currentFront());
        m_frontView->render(W - MARGIN * 2);
        scheduleRender();
    });
    connect(m_anki, &AnkiClient::currentBackChanged,     this, [this] {
        m_backView->setHtml(m_anki->currentBack());
        m_backView->render(W - MARGIN * 2);
        scheduleRender();
    });
    connect(m_anki, &AnkiClient::currentRemainingChanged,this, &Renderer::scheduleRender);
    connect(m_anki, &AnkiClient::statusMessageChanged,   this, &Renderer::scheduleRender);
    connect(m_anki, &AnkiClient::errorMessageChanged,    this, &Renderer::scheduleRender);
    connect(m_anki, &AnkiClient::cardsReviewedChanged,   this, &Renderer::scheduleRender);

    // Re-render when images finish loading
    connect(m_frontView, &HtmlView::imageLoaded, this, [this] {
        m_frontView->render(W - MARGIN * 2);
        scheduleRender();
    });
    connect(m_backView, &HtmlView::imageLoaded, this, [this] {
        m_backView->render(W - MARGIN * 2);
        scheduleRender();
    });

    // Pen thread signals
    connect(penThread, &PenThread::penFirstStroke, this, &Renderer::scheduleRender);

    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(0);
    connect(&m_renderTimer, &QTimer::timeout, this, [this] { render(); });

    scheduleRender();
}

void Renderer::scheduleRender()
{
    if (!m_renderTimer.isActive())
        m_renderTimer.start();
}

void Renderer::drawButton(QPainter &p,
                           const QRect &r, const QString &label,
                           const QColor &border, const QColor &bg, const QColor &fg,
                           int radius, int borderWidth, int fontSize)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(bg);
    p.setPen(QPen(border, borderWidth));
    p.drawRoundedRect(r, radius, radius);

    QFont f = p.font();
    f.setPixelSize(fontSize > 0 ? fontSize : FS_NORMAL);
    f.setBold(true);
    p.setFont(f);
    p.setPen(fg);
    p.drawText(r, Qt::AlignCenter, label);
    p.restore();
}

// Top-level render — m_canvas (clean card, no draw overlay).
// Draw overlay is composited by main.cpp before memcpy to shm.
void Renderer::render()
{
    m_hitRects.clear();
    m_canvas.fill(Qt::white);

    QPainter p(&m_canvas);
    p.setRenderHint(QPainter::TextAntialiasing);

    const QString &state = m_anki->currentState();

    if (state == QStringLiteral("LOGIN")) {
        drawLoginView(p);
    } else {
        QRect content(0, HDR_H, W, H - HDR_H - FTR_H);

        drawHeader(p);
        drawFooter(p);

        if (state == QStringLiteral("LOADING"))
            drawLoadingView(p, content);
        else if (state == QStringLiteral("ERROR"))
            drawErrorView(p, content);
        else if (state == QStringLiteral("DECKS"))
            drawDecksView(p, content);
        else if (state == QStringLiteral("DONE"))
            drawDoneView(p, content);
        else if (state == QStringLiteral("STUDY"))
            drawStudyView(p, content);
    }

    p.end();

    emit frameReady();
}

void Renderer::drawHeader(QPainter &p)
{
    const QString &state = m_anki->currentState();

    p.save();
    QFont f;

    if (state == QStringLiteral("STUDY") || state == QStringLiteral("DONE")) {
        QRect arrowR(MARGIN - 30, 0, 100, HDR_H);
        f.setPixelSize(FS_HEADER);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::black);
        p.drawText(arrowR, Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("\u2190"));
        addHit(arrowR, [this] { m_anki->loadDecks(); });
    }

    QString title;
    if (state == QStringLiteral("STUDY") || state == QStringLiteral("DONE"))
        title = m_anki->currentDeckName().left(30);
    else
        title = QStringLiteral("reMarkable Anki");

    QRect titleR(MARGIN + 80, 0, W - MARGIN * 2 - 200, HDR_H);
    f.setPixelSize(FS_HEADER);
    f.setBold(false);
    p.setFont(f);
    p.setPen(Qt::black);
    p.drawText(titleR, Qt::AlignVCenter | Qt::AlignLeft, title);

    if (state == QStringLiteral("STUDY")) {
        int total     = m_anki->currentTotal();
        int remaining = m_anki->currentRemaining();

        QRect pbArea(W - MARGIN - 420, 0, 420, HDR_H);
        int groupTop = (HDR_H - 68) / 2;

        QRect remR(pbArea.left(), groupTop, pbArea.width(), 44);
        f.setPixelSize(FS_SMALL);
        p.setFont(f);
        p.drawText(remR, Qt::AlignVCenter | Qt::AlignRight,
                   QStringLiteral("Remaining: ") + QString::number(remaining));

        QRect barOuter(pbArea.left(), groupTop + 44 + 8, pbArea.width(), 16);
        p.setPen(QPen(Qt::black, 2));
        p.setBrush(Qt::white);
        p.drawRect(barOuter);

        if (total > 0) {
            double pct = 1.0 - (double)remaining / total;
            int filled = (int)(barOuter.width() * pct);
            if (filled > 2) {
                QRect barFill = barOuter.adjusted(2, 2, -(barOuter.width() - filled) + 2, -2);
                p.setBrush(Qt::black);
                p.setPen(Qt::NoPen);
                p.drawRect(barFill);
            }
        }
    }

    p.setPen(QPen(Qt::black, 3));
    p.drawLine(0, HDR_H - 1, W, HDR_H - 1);

    p.restore();
}

void Renderer::drawFooter(QPainter &p)
{
    int footerTop = H - FTR_H;

    QRect sbR(0, footerTop, W, FTR_H);
    p.save();
    p.setPen(QPen(Qt::black, 3));
    p.drawLine(0, footerTop, W, footerTop);
    p.restore();

    QFont f; f.setPixelSize(FS_SMALL); p.setFont(f);
    p.setPen(Qt::black);
    p.drawText(sbR.adjusted(MARGIN, 0, 0, 0),
               Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("v1.0.0 - Jayy001 - ReMarkable"));

    QString clock = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
    QString connLabel;
    QColor  connColor;
    if      (m_anki->currentState() == QStringLiteral("ERROR"))
        { connLabel = QStringLiteral("Offline");   connColor = Qt::red; }
    else if (m_anki->currentState() == QStringLiteral("LOADING"))
        { connLabel = QStringLiteral("Syncing..."); connColor = QColor(255,140,0); }
    else
        { connLabel = QStringLiteral("Connected"); connColor = QColor(56,142,60); }

    QRect rightR = sbR.adjusted(0, 0, -MARGIN, 0);
    p.drawText(rightR, Qt::AlignVCenter | Qt::AlignRight,
               clock + QStringLiteral("  \u25CF  ") + connLabel);
    int dotX = W - MARGIN - 10 - p.fontMetrics().horizontalAdvance(connLabel) - 20;
    p.save();
    p.setBrush(connColor); p.setPen(Qt::NoPen);
    p.drawEllipse(dotX, footerTop + FTR_H/2 - 10, 20, 20);
    p.restore();
}

void Renderer::drawLoadingView(QPainter &p, const QRect &area)
{
    QFont f; f.setPixelSize(FS_LARGE); p.setFont(f);
    p.setPen(Qt::black);
    p.drawText(area, Qt::AlignCenter, m_anki->statusMessage());
}

void Renderer::drawErrorView(QPainter &p, const QRect &area)
{
    int cy = area.top() + area.height() / 2 - 100;
    QFont f;
    f.setPixelSize(FS_LARGE); f.setBold(true); p.setFont(f); p.setPen(Qt::black);
    p.drawText(QRect(MARGIN, cy, W - MARGIN * 2, 90),
               Qt::AlignCenter, QStringLiteral("Something went wrong"));

    f.setBold(false); f.setPixelSize(FS_NORMAL); p.setFont(f);
    p.drawText(QRect(MARGIN, cy + 110, W - MARGIN * 2, 120),
               Qt::AlignCenter | Qt::TextWordWrap, m_anki->errorMessage());

    QRect retryR((W - 400) / 2, cy + 260, 400, 110);
    drawButton(p, retryR, QStringLiteral("Retry"),
               Qt::black, Qt::white, Qt::black);
    addHit(retryR, [this] { m_anki->loadDecks(); });
}

void Renderer::drawDoneView(QPainter &p, const QRect &area)
{
    int cy = area.top() + area.height() / 2 - 120;
    QFont f; f.setPixelSize(FS_LARGE); f.setBold(true); p.setFont(f); p.setPen(Qt::black);
    p.drawText(QRect(MARGIN, cy, W - MARGIN * 2, 90),
               Qt::AlignCenter, QStringLiteral("Congratulations!"));

    f.setBold(false); f.setPixelSize(FS_NORMAL); p.setFont(f);
    p.drawText(QRect(MARGIN, cy + 110, W - MARGIN * 2, 70),
               Qt::AlignCenter, QStringLiteral("No more cards due in this deck."));

    if (m_anki->cardsReviewed() > 0) {
        p.drawText(QRect(MARGIN, cy + 190, W - MARGIN * 2, 70),
                   Qt::AlignCenter,
                   QStringLiteral("Reviewed ") +
                   QString::number(m_anki->cardsReviewed()) +
                   QStringLiteral(" card(s) this session."));
    }

    QRect backR((W - 500) / 2, cy + 290, 500, 110);
    drawButton(p, backR, QStringLiteral("Back to Decks"),
               Qt::black, Qt::white, Qt::black);
    addHit(backR, [this] { m_anki->loadDecks(); });
}

void Renderer::drawDecksView(QPainter &p, const QRect &area)
{
    static const int ROW_H   = 110;
    static const int LIST_HDR= 110;

    QFont f; f.setPixelSize(FS_NORMAL); f.setBold(true); p.setFont(f); p.setPen(Qt::black);
    p.drawText(QRect(MARGIN, area.top(), W / 2, LIST_HDR),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Deck"));

    QRect rightHdr(W - MARGIN - 550, area.top(), 550, LIST_HDR);
    int col = rightHdr.left();
    for (auto &hdr : {QStringLiteral("New"), QStringLiteral("Learn"), QStringLiteral("Due")}) {
        p.drawText(QRect(col, area.top(), 150, LIST_HDR),
                   Qt::AlignVCenter | Qt::AlignHCenter, hdr);
        col += 150 + 40;
    }

    p.save(); p.setPen(QPen(QColor(0,0,0,50), 2));
    p.drawLine(MARGIN, area.top() + LIST_HDR - 1,
               W - MARGIN, area.top() + LIST_HDR - 1);
    p.restore();

    const QVariantList &decks = m_anki->deckData();
    int yOff = area.top() + LIST_HDR - m_deckScrollOffset;

    for (int i = 0; i < decks.size(); ++i) {
        QVariantMap d = decks[i].toMap();
        if (!d[QStringLiteral("visible")].toBool()) continue;

        QRect rowR(0, yOff, W, ROW_H);
        if (yOff + ROW_H < area.top() || yOff > area.bottom())
            { yOff += ROW_H; continue; }

        QRect hitR = rowR.intersected(area);
        if (!hitR.isEmpty())
            addHit(hitR, [this, i] { m_anki->startStudy(i); });

        int indent = d[QStringLiteral("indent")].toInt();
        bool hasChildren = d[QStringLiteral("hasChildren")].toBool();
        bool collapsed   = d[QStringLiteral("collapsed")].toBool();
        int xLeft = MARGIN + indent * 60;

        if (hasChildren) {
            QRect chevR(xLeft, yOff, 55, ROW_H);
            f.setPixelSize(36); f.setBold(false); p.setFont(f);
            p.drawText(chevR, Qt::AlignVCenter | Qt::AlignHCenter,
                       collapsed ? QStringLiteral("\u25B6") : QStringLiteral("\u25BC"));
            HitRect rowHit = m_hitRects.back();
            m_hitRects.pop_back();
            addHit(chevR, [this, i] { m_anki->toggleDeck(i); });
            m_hitRects.push_back(rowHit);
            xLeft += 65;
        }

        QString title = d[QStringLiteral("title")].toString().left(30);
        f.setPixelSize(FS_NORMAL);
        f.setBold(indent == 0);
        p.setFont(f); p.setPen(Qt::black);
        p.drawText(QRect(xLeft, yOff, W / 2, ROW_H),
                   Qt::AlignVCenter | Qt::AlignLeft, title);

        int cx = W - MARGIN - 550;
        for (auto &key : {QStringLiteral("newC"), QStringLiteral("learnC"), QStringLiteral("dueC")}) {
            f.setBold(false); p.setFont(f);
            p.drawText(QRect(cx, yOff, 150, ROW_H),
                       Qt::AlignVCenter | Qt::AlignHCenter,
                       d[key].toString());
            cx += 190;
        }

        p.save(); p.setPen(QPen(QColor(0,0,0,25), 1));
        p.drawLine(MARGIN, yOff + ROW_H - 1, W - MARGIN, yOff + ROW_H - 1);
        p.restore();

        yOff += ROW_H;
    }
}

void Renderer::drawStudyView(QPainter &p, const QRect &area)
{
    int cardMargin = 60;
    int cardX      = cardMargin;
    int cardW      = W - cardMargin * 2;
    int cardPad    = 60;
    int deckNameH  = 60;

    int cardTop      = area.top() + 40;
    int contentX     = cardX + cardPad;
    int contentW     = cardW - cardPad * 2;
    int chromeTop    = cardPad + deckNameH + 20;

    int maxCardH     = area.height() - BTN_H - 60;
    int maxCardBot   = cardTop + maxCardH;
    int contentTopY  = cardTop + chromeTop;
    int maxContentH  = maxCardBot - cardPad - contentTopY;

    int contentH = 0;
    int frontH   = 0;
    int backH    = 0;
    int sepH     = 0;

    if (!m_answerRevealed) {
        frontH   = m_frontView->render(contentW);
        contentH = frontH;
    } else {
        frontH   = m_frontView->render(contentW);
        backH    = m_backView->render(contentW);
        sepH     = 20;
        contentH = frontH + sepH + backH;
    }

    int hintH = m_answerRevealed ? 0 : 50;

    double scale = 1.0;
    int    available = maxContentH - hintH;
    if (contentH > available && available > 0) {
        scale = static_cast<double>(available) / contentH;
        if (scale < 0.25) scale = 0.25;

        int virtualW = static_cast<int>(contentW / scale);
        frontH = m_frontView->render(virtualW);
        if (m_answerRevealed)
            backH = m_backView->render(virtualW);
        contentH = m_answerRevealed ? frontH + sepH + backH : frontH;
        scale = static_cast<double>(available) / contentH;
        if (scale < 0.25) scale = 0.25;
    }

    int drawnH = static_cast<int>(contentH * scale);

    int cardContentH = qMin(drawnH + hintH, maxContentH);
    int cardH        = chromeTop + cardContentH + cardPad;
    QRect cardR(cardX, cardTop, cardW, cardH);

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::white);
    p.setPen(QPen(Qt::black, 3));
    p.drawRoundedRect(cardR, 24, 24);
    p.restore();

    QFont f; f.setPixelSize(36); p.setFont(f);
    p.setPen(QColor(0,0,0,153));
    p.drawText(QRect(cardX + cardPad, cardR.top() + cardPad,
                     cardW - cardPad * 2, deckNameH),
               Qt::AlignLeft | Qt::AlignVCenter,
               m_anki->currentDeckName().left(30));

    const bool hasStrokes = m_penThread->hasStrokes();

    if (!m_answerRevealed) {
        p.save();
        p.translate(contentX, contentTopY);
        if (scale < 1.0) p.scale(scale, scale);
        m_frontView->draw(p, 0, 0);
        p.restore();

        int hintY = contentTopY + drawnH;
        QFont hf; hf.setPixelSize(FS_SMALL); p.setFont(hf);
        p.setPen(QColor(0,0,0,100));
        p.drawText(QRect(cardX, hintY, cardW, hintH),
                   Qt::AlignCenter, QStringLiteral("(tap to reveal answer)"));

        // Eraser hit registered before the reveal hitbox (first-match-wins)
        if (hasStrokes)
            addHit(m_eraserRect, [this] {
                {
                    std::lock_guard<std::mutex> lock(m_penThread->shmMutex());
                    m_penThread->clearStrokes();
                }
                scheduleRender();
            });

        QRect revealHit(0, contentTopY, W, area.bottom() - contentTopY);
        addHit(revealHit, [this] { m_answerRevealed = true; scheduleRender(); });
    } else {
        int frontDrawnH = static_cast<int>(frontH * scale);

        p.save();
        p.translate(contentX, contentTopY);
        if (scale < 1.0) p.scale(scale, scale);
        m_frontView->draw(p, 0, 0);
        p.restore();

        int sepY = contentTopY + frontDrawnH + 16;
        p.save();
        p.setPen(QPen(QColor(0,0,0,180), 3, Qt::SolidLine));
        p.drawLine(contentX, sepY, contentX + contentW, sepY);
        p.restore();

        int backTopY = sepY + 16;
        p.save();
        p.translate(contentX, backTopY);
        if (scale < 1.0) p.scale(scale, scale);
        m_backView->draw(p, 0, 0);
        p.restore();

        int areaBot   = area.top() + area.height();
        int btnGroupH = 160;
        int btnRowTop = maxCardBot + (areaBot - maxCardBot - btnGroupH) / 2 + 20;
        int btnCount  = qMin(m_anki->currentButtonLabels().size(), 4);
        if (btnCount == 0) btnCount = 4;

        static const QColor colors[] = {
            QColor("#D32F2F"), QColor("#F57C00"),
            QColor("#388E3C"), QColor("#0288D1")
        };
        static const char *names[] = { "Again", "Hard", "Good", "Easy" };

        int btnW    = 260;
        int spacing = 40;
        int totalBW = btnCount * btnW + (btnCount - 1) * spacing;
        int startX  = (W - totalBW) / 2;

        for (int i = 0; i < btnCount; ++i) {
            QColor col = colors[i % 4];
            QRect btnR(startX + i * (btnW + spacing), btnRowTop + 30, btnW, 110);

            QRect timeR = btnR.adjusted(0, -50, 0, -110);
            QFont tf; tf.setPixelSize(FS_SMALL); p.setFont(tf);
            p.setPen(Qt::black);
            QString timeStr = (i < m_anki->currentButtonLabels().size())
                              ? m_anki->currentButtonLabels().at(i) : QString();
            p.drawText(timeR, Qt::AlignCenter, timeStr);

            drawButton(p, btnR, QString::fromUtf8(names[i]),
                       col, Qt::white, col, 24, 5);

            int btn = i + 1;
            addHit(btnR, [this, btn] { m_anki->answerCard(btn); });
        }

        // Eraser hit registered before the reveal hitbox (first-match-wins)
        if (hasStrokes)
            addHit(m_eraserRect, [this] {
                {
                    std::lock_guard<std::mutex> lock(m_penThread->shmMutex());
                    m_penThread->clearStrokes();
                }
                scheduleRender();
            });
    }

    m_eraserRect = QRect(W - MARGIN - 200, area.top() + 10, 180, 70);
    if (hasStrokes) {
        drawButton(p, m_eraserRect, QStringLiteral("Erase"),
                   Qt::black, Qt::white, Qt::black, 16, 2, FS_SMALL);
    }
}

void Renderer::drawLoginView(QPainter &p)
{
    p.fillRect(0, 0, W, H, Qt::white);

    QFont f; f.setPixelSize(FS_LARGE); f.setBold(true); p.setFont(f); p.setPen(Qt::black);
    p.drawText(QRect(MARGIN, 100, W - MARGIN * 2, 100),
               Qt::AlignCenter, QStringLiteral("Sign in to AnkiWeb"));

    if (!m_anki->errorMessage().isEmpty()) {
        f.setBold(false); f.setPixelSize(FS_SMALL); p.setFont(f);
        p.setPen(QColor("#D32F2F"));
        p.drawText(QRect(MARGIN, 210, W - MARGIN * 2, 80),
                   Qt::AlignCenter | Qt::TextWordWrap, m_anki->errorMessage());
    }

    auto drawField = [&](const QString &label, const QString &value,
                          bool isFocused, bool isPassword, int yTop) {
        f.setBold(false); f.setPixelSize(FS_SMALL); p.setFont(f); p.setPen(Qt::black);
        p.drawText(QRect(MARGIN, yTop, W - MARGIN * 2, 50),
                   Qt::AlignLeft | Qt::AlignVCenter, label);

        QRect fieldR(MARGIN, yTop + 55, W - MARGIN * 2, 100);
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Qt::white);
        p.setPen(QPen(isFocused ? Qt::black : QColor("#999999"),
                      isFocused ? 4 : 2));
        p.drawRoundedRect(fieldR, 12, 12);

        f.setPixelSize(FS_NORMAL); p.setFont(f);
        p.setPen(value.isEmpty() ? QColor("#AAAAAA") : Qt::black);
        QString displayed = value.isEmpty()
            ? (isPassword ? QStringLiteral("password") : QStringLiteral("your@email.com"))
            : (isPassword ? QString(value.length(), QChar(0x2022)) : value);
        p.drawText(fieldR.adjusted(24, 0, -24, 0),
                   Qt::AlignVCenter | Qt::AlignLeft, displayed);
        p.restore();

        return fieldR;
    };

    int emailTop    = 300;
    int passwordTop = emailTop + 180;
    QRect emailR = drawField(QStringLiteral("Email"),    m_kbEmail,
                              m_kbFocusedField == 0, false, emailTop);
    QRect passR  = drawField(QStringLiteral("Password"), m_kbPassword,
                              m_kbFocusedField == 1, true,  passwordTop);
    addHit(emailR, [this] { m_kbFocusedField = 0; scheduleRender(); });
    addHit(passR,  [this] { m_kbFocusedField = 1; scheduleRender(); });

    static const char *lettersRow1[] = {"q","w","e","r","t","y","u","i","o","p"};
    static const char *lettersRow2[] = {"a","s","d","f","g","h","j","k","l"};
    static const char *lettersRow3[] = {"z","x","c","v","b","n","m"};
    static const char *symRow1[]     = {"1","2","3","4","5","6","7","8","9","0"};
    static const char *symRow2[]     = {"@","#","$","&","*","(",")","−","="};
    static const char *symRow3[]     = {"+","!","?","/","\\",":",";","'","\""};

    const char **r1 = m_kbSymbols ? symRow1 : lettersRow1;
    const char **r2 = m_kbSymbols ? symRow2 : lettersRow2;
    const char **r3 = m_kbSymbols ? symRow3 : lettersRow3;
    int r1Count = 10, r2Count = 9, r3Count = 7;

    int keyW = 130, keyH = 95, gap = 8;
    int kbTop = H - (keyH + gap) * 4 - 40;

    auto drawKey = [&](int kx, int ky, int kw, int kh,
                        const QString &label, std::function<void()> action,
                        bool highlighted = false) {
        QRect kr(kx, ky, kw, kh);
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(highlighted ? QColor("#CCCCCC") : Qt::white);
        p.setPen(QPen(Qt::black, 2));
        p.drawRoundedRect(kr, 10, 10);
        QFont kf; kf.setPixelSize(FS_KEY); p.setFont(kf); p.setPen(Qt::black);
        p.drawText(kr, Qt::AlignCenter, label);
        p.restore();
        addHit(kr, action);
    };

    {
        int totalW = 10 * keyW + 9 * gap;
        int x = (W - totalW) / 2;
        for (int i = 0; i < r1Count; ++i) {
            QString label = QString::fromUtf8(r1[i]);
            if (!m_kbSymbols && m_kbShifted) label = label.toUpper();
            QString lCopy = label;
            drawKey(x, kbTop, keyW, keyH, label,
                [this, lCopy] {
                    if (m_kbFocusedField == 0) m_kbEmail += lCopy;
                    else m_kbPassword += lCopy;
                    if (m_kbShifted) m_kbShifted = false;
                    scheduleRender();
                });
            x += keyW + gap;
        }
    }

    {
        int totalW = 9 * keyW + 8 * gap;
        int x = (W - totalW) / 2;
        int y = kbTop + keyH + gap;
        for (int i = 0; i < r2Count; ++i) {
            QString label = QString::fromUtf8(r2[i]);
            if (!m_kbSymbols && m_kbShifted) label = label.toUpper();
            QString lCopy = label;
            drawKey(x, y, keyW, keyH, label,
                [this, lCopy] {
                    if (m_kbFocusedField == 0) m_kbEmail += lCopy;
                    else m_kbPassword += lCopy;
                    if (m_kbShifted) m_kbShifted = false;
                    scheduleRender();
                });
            x += keyW + gap;
        }
    }

    {
        int totalW = 10 * keyW + 9 * gap;
        int x = (W - totalW) / 2;
        int y = kbTop + (keyH + gap) * 2;
        int shiftW = (totalW - 7 * keyW - 8 * gap) / 2 - gap;

        if (!m_kbSymbols) {
            drawKey(x, y, shiftW, keyH, QStringLiteral("SHIFT"),
                [this] { m_kbShifted = !m_kbShifted; scheduleRender(); },
                m_kbShifted);
        }
        x = (W - totalW) / 2 + shiftW + gap;

        for (int i = 0; i < r3Count; ++i) {
            QString label = QString::fromUtf8(r3[i]);
            if (!m_kbSymbols && m_kbShifted) label = label.toUpper();
            QString lCopy = label;
            drawKey(x, y, keyW, keyH, label,
                [this, lCopy] {
                    if (m_kbFocusedField == 0) m_kbEmail += lCopy;
                    else m_kbPassword += lCopy;
                    if (m_kbShifted) m_kbShifted = false;
                    scheduleRender();
                });
            x += keyW + gap;
        }

        int delX = (W - totalW) / 2 + totalW - shiftW;
        drawKey(delX, y, shiftW, keyH, QStringLiteral("DEL"),
            [this] {
                QString &field = (m_kbFocusedField == 0) ? m_kbEmail : m_kbPassword;
                if (!field.isEmpty()) field.chop(1);
                scheduleRender();
            });
    }

    {
        int totalW = 10 * keyW + 9 * gap;
        int y = kbTop + (keyH + gap) * 3;
        int x = (W - totalW) / 2;

        int symW  = 160, atW = 110, dotW = 110, signinW = 260;
        int spaceW = totalW - symW - atW - dotW - signinW - 4 * gap;

        drawKey(x, y, symW, keyH,
                m_kbSymbols ? QStringLiteral("ABC") : QStringLiteral("123"),
                [this] { m_kbSymbols = !m_kbSymbols; m_kbShifted = false; scheduleRender(); },
                m_kbSymbols);
        x += symW + gap;

        drawKey(x, y, atW, keyH, QStringLiteral("@"),
            [this] {
                if (m_kbFocusedField == 0) m_kbEmail += '@';
                else m_kbPassword += '@';
                scheduleRender();
            });
        x += atW + gap;

        drawKey(x, y, spaceW, keyH, QStringLiteral("space"),
            [this] {
                if (m_kbFocusedField == 0) m_kbEmail += ' ';
                else m_kbPassword += ' ';
                scheduleRender();
            });
        x += spaceW + gap;

        drawKey(x, y, dotW, keyH, QStringLiteral("."),
            [this] {
                if (m_kbFocusedField == 0) m_kbEmail += '.';
                else m_kbPassword += '.';
                scheduleRender();
            });
        x += dotW + gap;

        bool ready = !m_kbEmail.isEmpty() && !m_kbPassword.isEmpty();
        QRect signinR(x, y, signinW, keyH);
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(ready ? Qt::black : Qt::white);
        p.setPen(QPen(ready ? Qt::black : QColor("#999999"), 3));
        p.drawRoundedRect(signinR, 10, 10);
        QFont sf; sf.setPixelSize(44); sf.setBold(true); p.setFont(sf);
        p.setPen(ready ? Qt::white : QColor("#999999"));
        p.drawText(signinR, Qt::AlignCenter, QStringLiteral("Sign In"));
        p.restore();
        if (ready)
            addHit(signinR, [this] { m_anki->login(m_kbEmail, m_kbPassword); });
    }
}

void Renderer::handleTouch(int x, int y, int inputType)
{
    if (inputType == INPUT_TOUCH_PRESS) {
        m_touchDown = QPoint(x, y);
    } else if (inputType == INPUT_TOUCH_RELEASE) {
        if ((QPoint(x, y) - m_touchDown).manhattanLength() < 30) {
            for (const auto &hr : m_hitRects) {
                if (hr.rect.contains(x, y)) {
                    hr.action();
                    return;
                }
            }
        }
    } else if (inputType == INPUT_TOUCH_UPDATE) {
        if (m_anki->currentState() == QStringLiteral("DECKS")) {
            int dy = m_touchDown.y() - y;
            if (qAbs(dy) > 10) {
                m_deckScrollOffset = qMax(0, m_deckScrollOffset + dy / 4);
                m_touchDown.setY(y);
                scheduleRender();
            }
        }
    }
}
