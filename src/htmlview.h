#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPainter>
#include <QString>
#include <QTextDocument>
#include <QMap>

class HtmlView : public QObject
{
    Q_OBJECT

public:
    explicit HtmlView(QNetworkAccessManager *nam, QObject *parent = nullptr);
    ~HtmlView() = default;

    void setHtml(const QString &html, const QString &baseUrl = QString());
    int  render(int width);
    void draw(QPainter &p, int offsetX = 0, int offsetY = 0);
    void drawScaled(QPainter &p, int offsetX, int offsetY,
                    int availableWidth, int maxHeight);

    int documentHeight() const { return m_docHeight; }

signals:
    void imageLoaded();

private:
    QNetworkAccessManager        *m_nam;
    QTextDocument                 m_doc;
    QMap<QString, QNetworkReply*> m_inFlight;
    QMap<QString, QImage>         m_imageCache;

    int m_docHeight   = 0;
    int m_layoutWidth = 0;

    static QString preprocess(const QString &html);
    void loadImage(const QString &url);
};
