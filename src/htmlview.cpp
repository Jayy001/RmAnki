#include "htmlview.h"

#include <QAbstractTextDocumentLayout>
#include <QDebug>
#include <QFont>
#include <QNetworkRequest>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QUrl>

// Default stylesheet — large font, comfortable spacing, e-ink friendly
static const char *DEFAULT_CSS =
    "body  { font-size: 52px; font-family: sans-serif; margin: 0; padding: 0; }"
    "p     { margin: 0 0 16px 0; line-height: 1.4; }"
    "br    {}"
    "b, strong { font-weight: bold; }"
    "i, em     { font-style: italic; }"
    "u         { text-decoration: underline; }"
    "img       { max-width: 100%; }"
    "table     { border-collapse: collapse; width: 100%; }"
    "td, th    { padding: 8px 12px; border: 1px solid #888; }"
    "th        { font-weight: bold; background: #eee; }"
    "ul, ol    { margin: 8px 0 8px 32px; padding: 0; }"
    "li        { margin-bottom: 8px; line-height: 1.4; }"
    "h1 { font-size: 72px; font-weight: bold; margin: 0 0 12px 0; }"
    "h2 { font-size: 64px; font-weight: bold; margin: 0 0 10px 0; }"
    "h3 { font-size: 56px; font-weight: bold; margin: 0 0 8px 0; }"
    ".cloze    { text-decoration: underline; font-weight: bold; }";

/*static*/ QString HtmlView::preprocess(const QString &html)
{
    QString s = html;

    // 1. Strip <script>…</script>
    static const QRegularExpression reScript(
        QStringLiteral("<script[^>]*>.*?</script>"),
        QRegularExpression::CaseInsensitiveOption |
        QRegularExpression::DotMatchesEverythingOption);
    s.remove(reScript);

    // 2. Strip MathJax container tags and their content
    //    <mjx-container …>…</mjx-container>
    static const QRegularExpression reMjxTag(
        QStringLiteral("<mjx-[^>]*>.*?</mjx-[^>]*>"),
        QRegularExpression::CaseInsensitiveOption |
        QRegularExpression::DotMatchesEverythingOption);
    s.remove(reMjxTag);

    // 3. Strip LaTeX delimiters left by MathJax: \[…\]  \(…\)
    static const QRegularExpression reMathBlock(
        QStringLiteral("\\\\\\[.*?\\\\\\]"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression reMathInline(
        QStringLiteral("\\\\\\(.*?\\\\\\)"),
        QRegularExpression::DotMatchesEverythingOption);
    s.remove(reMathBlock);
    s.remove(reMathInline);

    // 4. Strip Anki TTS tags: {{tts …}} and [anki:tts …]…[/anki:tts]
    static const QRegularExpression reTtsCurly(
        QStringLiteral("\\{\\{tts[^}]*\\}\\}"));
    static const QRegularExpression reTtsAnki(
        QStringLiteral("\\[anki:tts[^\\]]*\\].*?\\[/anki:tts\\]"),
        QRegularExpression::DotMatchesEverythingOption);
    s.remove(reTtsCurly);
    s.remove(reTtsAnki);

    // 5. Evaluate Anki cloze: {{c\d+::answer}} or {{c\d+::answer::hint}}
    //    Show the answer text wrapped in a <span class="cloze">
    static const QRegularExpression reCloze(
        QStringLiteral("\\{\\{c\\d+::([^:}]+)(?:::[^}]*)?\\}\\}"));
    s.replace(reCloze, QStringLiteral("<span class=\"cloze\">\\1</span>"));

    return s;
}

HtmlView::HtmlView(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
{
    QFont f = m_doc.defaultFont();
    f.setPixelSize(52);
    m_doc.setDefaultFont(f);
    m_doc.setDefaultStyleSheet(QString::fromLatin1(DEFAULT_CSS));
}

void HtmlView::setHtml(const QString &html, const QString & /*baseUrl*/)
{
    m_docHeight   = 0;
    m_layoutWidth = 0;

    if (html.trimmed().isEmpty()) {
        m_doc.clear();
        return;
    }

    m_doc.setHtml(preprocess(html));

    // Kick off loading for any images referenced in the document
    for (QTextBlock b = m_doc.begin(); b != m_doc.end(); b = b.next()) {
        for (QTextBlock::Iterator it = b.begin(); !it.atEnd(); ++it) {
            QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            QTextImageFormat fmt = frag.charFormat().toImageFormat();
            if (fmt.isValid() && !fmt.name().isEmpty())
                loadImage(fmt.name());
        }
    }
}

int HtmlView::render(int width)
{
    if (width <= 0) return 0;
    m_layoutWidth = width;
    m_doc.setTextWidth(width);
    m_docHeight = static_cast<int>(m_doc.size().height());
    return m_docHeight;
}

void HtmlView::draw(QPainter &p, int offsetX, int offsetY)
{
    if (m_doc.isEmpty()) return;
    p.save();
    p.translate(offsetX, offsetY);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, Qt::black);
    m_doc.documentLayout()->draw(&p, ctx);
    p.restore();
}

void HtmlView::drawScaled(QPainter &p, int offsetX, int offsetY,
                           int availableWidth, int maxHeight)
{
    if (availableWidth <= 0 || maxHeight <= 0) return;

    // Ensure layout is current for this width
    if (m_layoutWidth != availableWidth)
        render(availableWidth);

    int docH = m_docHeight;
    double scale = 1.0;

    if (docH > maxHeight) {
        scale = static_cast<double>(maxHeight) / docH;
        if (scale < 0.3) scale = 0.3;

        // Re-layout at a wider virtual width so text wraps less after scaling
        int virtualW = static_cast<int>(availableWidth / scale);
        m_doc.setTextWidth(virtualW);
        m_layoutWidth = virtualW;
        docH = static_cast<int>(m_doc.size().height());
        m_docHeight = docH;
        scale = static_cast<double>(maxHeight) / docH;
        if (scale < 0.3) scale = 0.3;
    }

    p.save();
    p.translate(offsetX, offsetY);
    if (scale < 1.0)
        p.scale(scale, scale);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, Qt::black);
    m_doc.documentLayout()->draw(&p, ctx);
    p.restore();
}

void HtmlView::loadImage(const QString &url)
{
    if (url.isEmpty()) return;
    if (m_imageCache.contains(url)) {
        m_doc.addResource(QTextDocument::ImageResource, QUrl(url),
                          QVariant(m_imageCache[url]));
        return;
    }
    if (m_inFlight.contains(url)) return;

    if (url.startsWith(QLatin1String("data:"))) {
        int comma = url.indexOf(',');
        if (comma > 0) {
            QByteArray data = url.contains(QLatin1String("base64"))
                ? QByteArray::fromBase64(url.mid(comma + 1).toLatin1())
                : QByteArray::fromPercentEncoding(url.mid(comma + 1).toLatin1());
            QImage img;
            img.loadFromData(data);
            if (!img.isNull()) {
                m_imageCache[url] = img;
                m_doc.addResource(QTextDocument::ImageResource, QUrl(url),
                                  QVariant(img));
            }
        }
        return;
    }

    if (!m_nam) return;

    QNetworkReply *reply = m_nam->get(QNetworkRequest(QUrl(url)));
    m_inFlight[url] = reply;

    connect(reply, &QNetworkReply::finished, this, [this, url, reply]() {
        reply->deleteLater();
        m_inFlight.remove(url);
        if (reply->error() == QNetworkReply::NoError) {
            QImage img;
            img.loadFromData(reply->readAll());
            if (!img.isNull()) {
                m_imageCache[url] = img;
                m_doc.addResource(QTextDocument::ImageResource, QUrl(url),
                                  QVariant(img));
                if (m_layoutWidth > 0) {
                    m_doc.setTextWidth(m_layoutWidth);
                    m_docHeight = static_cast<int>(m_doc.size().height());
                }
                emit imageLoaded();
            }
        }
    });
}
