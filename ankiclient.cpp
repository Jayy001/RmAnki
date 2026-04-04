#include "ankiclient.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDateTime>
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <functional>

// ============================================================================
// Constants
// ============================================================================

static const QByteArray DECK_BASE  = "https://ankiweb.net";
static const QByteArray STUDY_BASE = "https://ankiuser.net";
static const QByteArray USER_AGENT =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/144.0.0.0 Safari/537.36";

// ============================================================================
// Protobuf wire-format codec (no external library)
// ============================================================================

namespace {

// ---- Reader ----------------------------------------------------------------

struct PbReader {
    const uint8_t *buf;
    int len;
    int pos;
};

inline PbReader pbMakeReader(const QByteArray &data) {
    return { reinterpret_cast<const uint8_t *>(data.constData()),
             static_cast<int>(data.size()), 0 };
}

uint64_t pbReadVarint(PbReader &r) {
    uint64_t result = 0;
    int shift = 0;
    for (int i = 0; i < 10 && r.pos < r.len; ++i) {
        uint8_t b = r.buf[r.pos++];
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return result;
        shift += 7;
    }
    return result;
}

int64_t pbReadSigned(PbReader &r) {
    return static_cast<int64_t>(pbReadVarint(r));
}

QByteArray pbReadBytes(PbReader &r, int len) {
    QByteArray out(reinterpret_cast<const char *>(r.buf + r.pos), len);
    r.pos += len;
    return out;
}

QString pbReadString(PbReader &r, int len) {
    QString out = QString::fromUtf8(reinterpret_cast<const char *>(r.buf + r.pos), len);
    r.pos += len;
    return out;
}

void pbSkip(PbReader &r, int wireType) {
    switch (wireType) {
    case 0: // varint
        while (r.pos < r.len && (r.buf[r.pos++] & 0x80)) {}
        break;
    case 1: r.pos += 8; break; // 64-bit
    case 2: { int len = static_cast<int>(pbReadVarint(r)); r.pos += len; } break;
    case 5: r.pos += 4; break; // 32-bit
    }
}

// ---- Writer ----------------------------------------------------------------

void pbWriteVarint(QByteArray &out, uint64_t value) {
    while (value > 0x7F) {
        out.append(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.append(static_cast<char>(value & 0x7F));
}

void pbWriteTag(QByteArray &out, int field, int wireType) {
    pbWriteVarint(out, static_cast<uint64_t>((field << 3) | wireType));
}

void pbWriteVarintField(QByteArray &out, int field, uint64_t value) {
    if (value == 0) return; // proto3 default — omit
    pbWriteTag(out, field, 0);
    pbWriteVarint(out, value);
}

void pbWriteSignedField(QByteArray &out, int field, int64_t value) {
    if (value == 0) return;
    pbWriteTag(out, field, 0);
    pbWriteVarint(out, static_cast<uint64_t>(value)); // two's complement
}

void pbWriteMessage(QByteArray &out, int field, const QByteArray &msg) {
    if (msg.isEmpty()) return;
    pbWriteTag(out, field, 2);
    pbWriteVarint(out, static_cast<uint64_t>(msg.size()));
    out.append(msg);
}

// ---- Decoders --------------------------------------------------------------

struct DeckTreeNode {
    int64_t  deck_id = 0;
    QString  name;
    QList<DeckTreeNode> children;
    uint32_t level        = 0;
    bool     collapsed    = false;
    uint32_t review_count = 0;
    uint32_t learn_count  = 0;
    uint32_t new_count    = 0;
    uint32_t total_in_deck = 0;
    uint32_t total_including_children = 0;
    bool     filtered     = false;
};

DeckTreeNode decodeDeckTreeNode(const QByteArray &data) {
    PbReader r = pbMakeReader(data);
    DeckTreeNode node;
    while (r.pos < r.len) {
        uint64_t tag = pbReadVarint(r);
        int fn = static_cast<int>(tag >> 3);
        int wt = static_cast<int>(tag & 7);
        switch (fn) {
        case 1: if (wt==0) node.deck_id = pbReadSigned(r); else pbSkip(r,wt); break;
        case 2: if (wt==2) { int l=static_cast<int>(pbReadVarint(r)); node.name = pbReadString(r,l); } else pbSkip(r,wt); break;
        case 3: if (wt==2) { int l=static_cast<int>(pbReadVarint(r)); node.children.append(decodeDeckTreeNode(pbReadBytes(r,l))); } else pbSkip(r,wt); break;
        case 4: if (wt==0) node.level = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 5: if (wt==0) node.collapsed = pbReadVarint(r) != 0; else pbSkip(r,wt); break;
        case 6: if (wt==0) node.review_count = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 7: if (wt==0) node.learn_count = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 8: if (wt==0) node.new_count = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 13: if (wt==0) node.total_in_deck = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 14: if (wt==0) node.total_including_children = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 16: if (wt==0) node.filtered = pbReadVarint(r) != 0; else pbSkip(r,wt); break;
        default: pbSkip(r, wt); break;
        }
    }
    return node;
}

DeckTreeNode decodeDeckListInfoResponse(const QByteArray &data) {
    PbReader r = pbMakeReader(data);
    DeckTreeNode root;
    while (r.pos < r.len) {
        uint64_t tag = pbReadVarint(r);
        int fn = static_cast<int>(tag >> 3);
        int wt = static_cast<int>(tag & 7);
        if (fn == 1 && wt == 2) {
            int len = static_cast<int>(pbReadVarint(r));
            root = decodeDeckTreeNode(pbReadBytes(r, len));
        } else {
            pbSkip(r, wt);
        }
    }
    return root;
}

void extractRawStates(const QByteArray &data, StudyCard &card) {
    PbReader r = pbMakeReader(data);
    while (r.pos < r.len) {
        uint64_t tag = pbReadVarint(r);
        int fn = static_cast<int>(tag >> 3);
        int wt = static_cast<int>(tag & 7);
        if (wt == 2) {
            int len = static_cast<int>(pbReadVarint(r));
            QByteArray raw = pbReadBytes(r, len);
            switch (fn) {
            case 1: card.nextStates_current = raw; break;
            case 2: card.nextStates_again   = raw; break;
            case 3: card.nextStates_hard    = raw; break;
            case 4: card.nextStates_good    = raw; break;
            case 5: card.nextStates_easy    = raw; break;
            default: break;
            }
        } else {
            pbSkip(r, wt);
        }
    }
}

StudyCard decodeStudyCard(const QByteArray &data) {
    PbReader r = pbMakeReader(data);
    StudyCard card;
    card.card_id = 0;
    card.count_index = 0;
    card.note_id = 0;
    card.template_index = 0;
    while (r.pos < r.len) {
        uint64_t tag = pbReadVarint(r);
        int fn = static_cast<int>(tag >> 3);
        int wt = static_cast<int>(tag & 7);
        switch (fn) {
        case 1: if (wt==0) card.card_id = pbReadSigned(r); else pbSkip(r,wt); break;
        case 2: if (wt==2) { int l=static_cast<int>(pbReadVarint(r)); card.question = pbReadString(r,l); } else pbSkip(r,wt); break;
        case 3: if (wt==2) { int l=static_cast<int>(pbReadVarint(r)); card.answer = pbReadString(r,l); } else pbSkip(r,wt); break;
        case 4: if (wt==0) card.count_index = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 5: if (wt==2) { int l=static_cast<int>(pbReadVarint(r)); card.button_labels.append(pbReadString(r,l)); } else pbSkip(r,wt); break;
        case 6: if (wt==0) card.note_id = pbReadSigned(r); else pbSkip(r,wt); break;
        case 7: if (wt==0) card.template_index = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 8: if (wt==2) { int l=static_cast<int>(pbReadVarint(r)); extractRawStates(pbReadBytes(r,l), card); } else pbSkip(r,wt); break;
        default: pbSkip(r, wt); break;
        }
    }
    return card;
}

StudyResponse decodeStudyCardsResponse(const QByteArray &data) {
    PbReader r = pbMakeReader(data);
    StudyResponse resp;
    resp.sched_ver = 0;
    resp.new_count = 0;
    resp.learn_count = 0;
    resp.review_count = 0;
    while (r.pos < r.len) {
        uint64_t tag = pbReadVarint(r);
        int fn = static_cast<int>(tag >> 3);
        int wt = static_cast<int>(tag & 7);
        switch (fn) {
        case 1: if (wt==0) resp.sched_ver = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 2: if (wt==2) { int l=static_cast<int>(pbReadVarint(r)); resp.cards.append(decodeStudyCard(pbReadBytes(r,l))); } else pbSkip(r,wt); break;
        case 3: if (wt==0) resp.new_count = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 4: if (wt==0) resp.learn_count = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        case 5: if (wt==0) resp.review_count = static_cast<uint32_t>(pbReadVarint(r)); else pbSkip(r,wt); break;
        default: pbSkip(r, wt); break;
        }
    }
    return resp;
}

// ---- Login response decoder ------------------------------------------------

QString decodeLoginResponse(const QByteArray &data) {
    // Extract the first string field (wire type 2) from the login response
    // protobuf. This is the session token.
    PbReader r = pbMakeReader(data);
    while (r.pos < r.len) {
        uint64_t tag = pbReadVarint(r);
        int fn = static_cast<int>(tag >> 3);
        int wt = static_cast<int>(tag & 7);
        if (wt == 2) {
            int len = static_cast<int>(pbReadVarint(r));
            if (fn == 2) {
                // Field 2 = session token
                return pbReadString(r, len);
            }
            r.pos += len; // skip other length-delimited fields
        } else {
            pbSkip(r, wt);
        }
    }
    return QString();
}

// ---- Login request encoder -------------------------------------------------

QByteArray encodeLoginRequest(const QString &email, const QString &password) {
    QByteArray out;
    QByteArray emailBytes = email.toUtf8();
    QByteArray passBytes  = password.toUtf8();
    pbWriteMessage(out, 1, emailBytes);
    pbWriteMessage(out, 2, passBytes);
    return out;
}

// ---- Encoders --------------------------------------------------------------

QByteArray encodeSimpleI64(int64_t value) {
    QByteArray out;
    // field 1, wire type 0 (varint)
    pbWriteTag(out, 1, 0);
    pbWriteVarint(out, static_cast<uint64_t>(value));
    return out;
}

QByteArray encodeSimpleU64(uint64_t value) {
    QByteArray out;
    pbWriteVarintField(out, 1, value);
    return out;
}

QByteArray encodeAnswerCardRequest(const StudyCard &card, int button,
                                    int64_t nextCardId)
{
    // Determine which next-state bytes to use
    QByteArray nextRaw;
    switch (button) {
    case 1: nextRaw = card.nextStates_again; break;
    case 2: nextRaw = card.nextStates_hard;  break;
    case 3: nextRaw = card.nextStates_good;  break;
    case 4: nextRaw = card.nextStates_easy;  break;
    }

    // Build AnswerInfo sub-message
    QByteArray answerInfo;
    pbWriteSignedField(answerInfo, 1, card.card_id);
    pbWriteVarintField(answerInfo, 2, static_cast<uint64_t>(button));
    pbWriteVarintField(answerInfo, 3, 10000); // time_taken_millis
    pbWriteSignedField(answerInfo, 4, QDateTime::currentMSecsSinceEpoch());
    pbWriteMessage(answerInfo, 5, card.nextStates_current);
    pbWriteMessage(answerInfo, 6, nextRaw);

    // Build outer AnswerCardRequest
    QByteArray out;
    pbWriteMessage(out, 1, answerInfo);
    if (nextCardId != 0)
        pbWriteSignedField(out, 2, nextCardId);
    return out;
}

// ---- HTML stripping --------------------------------------------------------

QString stripHtml(const QString &text) {
    static const QRegularExpression reStyle(
        QStringLiteral("<style[^>]*>.*?</style>"),
        QRegularExpression::DotMatchesEverythingOption |
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reHr(
        QStringLiteral("<hr[^>]*/?>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reBr(
        QStringLiteral("<br\\s*/?>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reTags(
        QStringLiteral("<[^>]+>"));
    static const QRegularExpression reNumEntity(
        QStringLiteral("&#(\\d+);"));
    static const QRegularExpression reHexEntity(
        QStringLiteral("&#x([0-9a-fA-F]+);"));

    QString s = text;
    // Remove <style> blocks
    s.replace(reStyle, QString());
    // <hr> → newline separator
    s.replace(reHr, QStringLiteral("\n---\n"));
    // <br> → newline
    s.replace(reBr, QStringLiteral("\n"));
    // Strip remaining tags
    s.replace(reTags, QString());
    // Decode common HTML entities
    s.replace(QStringLiteral("&amp;"),  QStringLiteral("&"));
    s.replace(QStringLiteral("&lt;"),   QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"),   QStringLiteral(">"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&#39;"),  QStringLiteral("'"));
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    // Numeric entities: &#NNN;
    {
        QRegularExpressionMatchIterator it = reNumEntity.globalMatch(s);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            s.replace(m.captured(0), QString(QChar(m.captured(1).toInt())));
        }
    }
    // Hex entities: &#xHHH;
    {
        QRegularExpressionMatchIterator it = reHexEntity.globalMatch(s);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            bool ok;
            s.replace(m.captured(0), QString(QChar(m.captured(1).toInt(&ok, 16))));
        }
    }
    return s.trimmed();
}

// ---- Flatten deck tree for QML model ---------------------------------------

void flattenDeckTree(const DeckTreeNode &node, QList<DeckInfo> &list, int indent) {
    if (node.deck_id != 0 || !node.name.isEmpty()) {
        DeckInfo d;
        d.title       = node.name.isEmpty() ? QStringLiteral("(root)") : node.name;
        d.deck_id     = node.deck_id;
        d.newC        = static_cast<int>(node.new_count);
        d.learnC      = static_cast<int>(node.learn_count);
        d.dueC        = static_cast<int>(node.review_count);
        d.indent      = indent;
        d.hasChildren = !node.children.isEmpty();
        d.totalDue    = d.newC + d.learnC + d.dueC;
        list.append(d);
    }
    for (const auto &child : node.children) {
        flattenDeckTree(child, list, indent + 1);
    }
}

// ---- Common request setup --------------------------------------------------

QNetworkRequest makePostRequest(const QUrl &url,
                                const QByteArray &cookie = {},
                                const QByteArray &origin = {},
                                const QByteArray &referer = {})
{
    QNetworkRequest req{url};
    req.setRawHeader("Content-Type",     "application/octet-stream");
    req.setRawHeader("Accept",           "*/*");
    if (!cookie.isEmpty())
        req.setRawHeader("Cookie",       cookie);
    if (!origin.isEmpty())
        req.setRawHeader("Origin",       origin);
    if (!referer.isEmpty())
        req.setRawHeader("Referer",      referer);
    req.setRawHeader("User-Agent",       USER_AGENT);
    req.setRawHeader("pragma",           "no-cache");
    req.setRawHeader("cache-control",    "no-cache");
    return req;
}

} // anonymous namespace

// ============================================================================
// AnkiClient implementation
// ============================================================================

AnkiClient::AnkiClient(QObject *parent)
    : QObject(parent)
    , m_currentState(QStringLiteral("LOGIN"))
{
    // Try to restore saved session cookies from separate files
    m_ankiwebCookie  = loadToken(QStringLiteral(".ankiweb-jwt"));
    m_ankiuserCookie = loadToken(QStringLiteral(".ankiuser-jwt"));

    if (!m_ankiwebCookie.isEmpty() && !m_ankiuserCookie.isEmpty()) {
        qDebug() << "Restored cookies — ankiweb:" << m_ankiwebCookie.size()
                 << "bytes, ankiuser:" << m_ankiuserCookie.size() << "bytes";
        // Skip login screen — go straight to loading decks.
        QMetaObject::invokeMethod(this, &AnkiClient::loadDecks, Qt::QueuedConnection);
    }
}

// ---- State helpers ---------------------------------------------------------

void AnkiClient::setCurrentState(const QString &s) {
    if (m_currentState != s) {
        m_currentState = s;
        emit currentStateChanged();
    }
}

void AnkiClient::setStatusMessage(const QString &s) {
    if (m_statusMessage != s) {
        m_statusMessage = s;
        emit statusMessageChanged();
    }
}

void AnkiClient::setErrorMessage(const QString &s) {
    if (m_errorMessage != s) {
        m_errorMessage = s;
        emit errorMessageChanged();
    }
}

void AnkiClient::setError(const QString &msg) {
    setErrorMessage(msg);
    setCurrentState(QStringLiteral("ERROR"));
}

// ---- Card content cache ----------------------------------------------------

void AnkiClient::cacheStudyCards()
{
    // First pass: cache any card that has content
    for (const auto &card : m_study.cards) {
        if (!card.question.isEmpty())
            m_cardCache[card.card_id] = card;
    }

    // Second pass: fill in stubs (cards with empty question) from cache
    for (int i = 0; i < m_study.cards.size(); ++i) {
        StudyCard &card = m_study.cards[i];
        if (card.question.isEmpty() && m_cardCache.contains(card.card_id)) {
            const StudyCard &cached = m_cardCache[card.card_id];
            card.question = cached.question;
            card.answer   = cached.answer;
            // Keep the fresh scheduling data (next_states, button_labels)
            // from the server response — only fill in display content
        }
    }
}

// ---- Deck collapse / rebuild -----------------------------------------------

void AnkiClient::rebuildDeckData()
{
    QVariantList vl;
    for (int i = 0; i < m_decks.size(); ++i) {
        const DeckInfo &d = m_decks[i];

        // A deck is visible if none of its ancestors are collapsed.
        // Walk backwards through the flat list finding each ancestor
        // (first item with a strictly lower indent level).
        bool vis = true;
        int targetIndent = d.indent;
        for (int j = i - 1; j >= 0 && targetIndent > 0; --j) {
            if (m_decks[j].indent < targetIndent) {
                if (m_collapsedIndices.contains(j)) {
                    vis = false;
                    break;
                }
                targetIndent = m_decks[j].indent;
            }
        }

        QVariantMap m;
        m[QStringLiteral("title")]       = d.title;
        m[QStringLiteral("newC")]        = d.newC;
        m[QStringLiteral("learnC")]      = d.learnC;
        m[QStringLiteral("dueC")]        = d.dueC;
        m[QStringLiteral("indent")]      = d.indent;
        m[QStringLiteral("hasChildren")] = d.hasChildren;
        m[QStringLiteral("totalDue")]    = d.totalDue;
        m[QStringLiteral("collapsed")]   = m_collapsedIndices.contains(i);
        m[QStringLiteral("visible")]     = vis;
        vl.append(m);
    }
    m_deckData = vl;
    emit deckDataChanged();
}

void AnkiClient::toggleDeck(int index)
{
    if (index < 0 || index >= m_decks.size()) return;
    if (!m_decks[index].hasChildren) return;

    if (m_collapsedIndices.contains(index))
        m_collapsedIndices.remove(index);
    else
        m_collapsedIndices.insert(index);

    rebuildDeckData();
}

// ---- Networking ------------------------------------------------------------

void AnkiClient::handleAuthFailure()
{
    m_ankiwebCookie.clear();
    m_ankiuserCookie.clear();
    // Delete persisted tokens
    QFile::remove(QDir::homePath() + QStringLiteral("/.ankiweb-jwt"));
    QFile::remove(QDir::homePath() + QStringLiteral("/.ankiuser-jwt"));
    setErrorMessage(QStringLiteral("Session expired. Please log in again."));
    setCurrentState(QStringLiteral("LOGIN"));
}

QByteArray AnkiClient::extractSetCookie(QNetworkReply *reply)
{
    // Qt may combine multiple Set-Cookie headers into one value separated
    // by \n.  Split on \n and check each individual cookie.
    for (const auto &pair : reply->rawHeaderPairs()) {
        if (pair.first.toLower() != "set-cookie")
            continue;
        const QList<QByteArray> cookies = pair.second.split('\n');
        for (const QByteArray &sc : cookies) {
            QByteArray trimmed = sc.trimmed();
            if (!trimmed.startsWith("ankiweb="))
                continue;
            // Return "ankiweb=<token>" (strip path/domain after semicolon)
            int semiIdx = trimmed.indexOf(';');
            return (semiIdx > 0) ? trimmed.left(semiIdx) : trimmed;
        }
    }
    return QByteArray();
}

void AnkiClient::saveToken(const QString &filename, const QByteArray &cookie)
{
    QString path = QDir::homePath() + "/" + filename;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(cookie);
        f.close();
        qDebug() << "Saved cookie to" << path;
    } else {
        qDebug() << "Warning: could not save cookie to" << path;
    }
}

QByteArray AnkiClient::loadToken(const QString &filename)
{
    QString path = QDir::homePath() + "/" + filename;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QByteArray data = f.readAll().trimmed();
        f.close();
        return data;
    }
    return QByteArray();
}

void AnkiClient::post(const QString &url, const QByteArray &body,
                       const QByteArray &cookie, const QByteArray &origin,
                       const QByteArray &referer,
                       std::function<void(bool, QByteArray)> callback)
{
    QNetworkRequest req = makePostRequest(QUrl(url), cookie, origin, referer);

    QNetworkReply *reply = m_nam.post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 403) {
            qDebug() << "Got 403 — session expired, forcing re-login";
            handleAuthFailure();
            return;
        }
        if (reply->error() == QNetworkReply::NoError) {
            callback(true, reply->readAll());
        } else {
            callback(false, QByteArray("HTTP error: ") +
                     QByteArray::number(status) +
                     " " + reply->errorString().toUtf8());
        }
    });
}

void AnkiClient::postDecks(const QString &path, const QByteArray &body,
                             std::function<void(bool, QByteArray)> callback)
{
    post(QString::fromLatin1(DECK_BASE) + path, body,
         m_ankiwebCookie, DECK_BASE, DECK_BASE + "/decks", callback);
}

void AnkiClient::postStudy(const QString &path, const QByteArray &body,
                             std::function<void(bool, QByteArray)> callback)
{
    post(QString::fromLatin1(STUDY_BASE) + path, body,
         m_ankiuserCookie, STUDY_BASE, STUDY_BASE + "/study", callback);
}

// ---- Auth flow -------------------------------------------------------------
// Step 1: POST /svc/account/login → get JWT token (protobuf field 2)
//         + capture Set-Cookie for ankiweb.net
// Step 2: Set m_ankiwebCookie from that Set-Cookie header
// Step 3: GET ankiuser.net/account/ankiuser-login?t=<jwt> (with ankiweb cookie)
// Step 4: Set m_ankiuserCookie from that Set-Cookie header

void AnkiClient::completeAnkiuserLogin(const QString &jwtToken)
{
    // Step 3: GET to ankiuser.net to obtain ankiuser cookie.
    // The cookie for this request is the JWT token from the protobuf body
    // (the "aul" token), NOT the Set-Cookie from step 1.
    // The response is a 303 redirect — we must disable auto-redirect
    // so we can capture the Set-Cookie header from the 303 itself.
    QString url = QStringLiteral("https://ankiuser.net/account/ankiuser-login?t=")
                  + jwtToken;

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Cookie",      "ankiweb=" + jwtToken.toUtf8());
    req.setRawHeader("User-Agent",  USER_AGENT);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);

    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        qDebug() << "ankiuser-login status:" << status;

        // Step 4: extract Set-Cookie for ankiuser.net from the 303 response
        QByteArray ankiuserCookie = extractSetCookie(reply);

        if (ankiuserCookie.isEmpty()) {
            qDebug() << "Warning: no ankiuser Set-Cookie found, headers:";
            for (const auto &p : reply->rawHeaderPairs())
                qDebug() << "  " << p.first << ":" << p.second;
            setErrorMessage(QStringLiteral("Login failed: could not obtain ankiuser cookie"));
            setCurrentState(QStringLiteral("LOGIN"));
            return;
        }

        m_ankiuserCookie = ankiuserCookie;
        qDebug() << "ankiuser cookie set:" << m_ankiuserCookie.left(60) << "...";

        saveToken(QStringLiteral(".ankiuser-jwt"), m_ankiuserCookie);

        loadDecks();
    });
}

void AnkiClient::login(const QString &email, const QString &password)
{
    setErrorMessage(QString());
    setCurrentState(QStringLiteral("LOADING"));
    setStatusMessage(QStringLiteral("Signing in..."));

    QByteArray body = encodeLoginRequest(email, password);

    // Step 1: POST login — we need the raw reply to read Set-Cookie headers
    QNetworkRequest req = makePostRequest(
        QUrl(QStringLiteral("https://ankiweb.net/svc/account/login")),
        /*cookie=*/ {},
        /*origin=*/ "https://ankiweb.net",
        /*referer=*/ "https://ankiweb.net/account/login");

    QNetworkReply *reply = m_nam.post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            setErrorMessage(QStringLiteral("Login failed: HTTP ") +
                           QString::number(status) + " " +
                           reply->errorString());
            setCurrentState(QStringLiteral("LOGIN"));
            return;
        }

        QByteArray data = reply->readAll();

        // Extract JWT token from protobuf field 2
        QString jwtToken = decodeLoginResponse(data);
        if (jwtToken.isEmpty()) {
            setErrorMessage(QStringLiteral("Login failed: could not extract token"));
            setCurrentState(QStringLiteral("LOGIN"));
            return;
        }
        qDebug() << "Login OK, JWT length:" << jwtToken.length();

        // Step 2: capture Set-Cookie header for ankiweb.net
        QByteArray ankiwebCookie = extractSetCookie(reply);
        if (ankiwebCookie.isEmpty()) {
            // Fallback: build cookie from the JWT token directly
            ankiwebCookie = "ankiweb=" + jwtToken.toUtf8();
            qDebug() << "No Set-Cookie from login, using JWT token as ankiweb cookie";
        } else {
            qDebug() << "ankiweb cookie from Set-Cookie:" << ankiwebCookie.left(60) << "...";
        }
        m_ankiwebCookie = ankiwebCookie;
        saveToken(QStringLiteral(".ankiweb-jwt"), m_ankiwebCookie);

        // Steps 3-4: GET ankiuser-login to obtain ankiuser cookie
        completeAnkiuserLogin(jwtToken);
    });
}

void AnkiClient::loadDecks()
{
    setCurrentState(QStringLiteral("LOADING"));
    setStatusMessage(QStringLiteral("Loading decks..."));

    QByteArray body = encodeSimpleI64(-60);

    postDecks(QStringLiteral("/svc/decks/deck-list-info"), body,
              [this](bool ok, QByteArray data) {
        if (!ok) { setError(QString::fromUtf8(data)); return; }

        DeckTreeNode root = decodeDeckListInfoResponse(data);

        // Flatten into parallel lists
        m_decks.clear();
        flattenDeckTree(root, m_decks, 0);

        m_collapsedIndices.clear();
        rebuildDeckData();

        setCurrentState(QStringLiteral("DECKS"));
    });
}

void AnkiClient::startStudy(int index)
{
    if (index < 0 || index >= m_decks.size()) {
        setError(QStringLiteral("Invalid deck index"));
        return;
    }

    const DeckInfo &deck = m_decks[index];
    m_currentDeckName = deck.title;
    emit currentDeckNameChanged();

    m_cardsReviewed = 0;
    emit cardsReviewedChanged();

    setCurrentState(QStringLiteral("LOADING"));
    setStatusMessage(QStringLiteral("Selecting deck..."));

    // Step 1: select deck
    QByteArray selectBody = encodeSimpleU64(static_cast<uint64_t>(deck.deck_id));

    postDecks(QStringLiteral("/svc/decks/select-deck"), selectBody,
              [this](bool ok, QByteArray data) {
        if (!ok) { setError(QString::fromUtf8(data)); return; }

        // Step 2: fetch study cards
        postStudy(QStringLiteral("/svc/study/study-cards"), QByteArray(),
                  [this](bool ok2, QByteArray data2) {
            if (!ok2) { setError(QString::fromUtf8(data2)); return; }

            qDebug() << "initial study-cards response:" << data2.size() << "bytes";

            m_cardCache.clear();  // new study session
            m_study = decodeStudyCardsResponse(data2);
            cacheStudyCards();
            int totalDue = m_study.totalDue();
            m_currentTotal = totalDue;
            emit currentTotalChanged();

            if (m_study.cards.isEmpty()) {
                setCurrentState(QStringLiteral("DONE"));
                return;
            }
            showNextCard();
        });
    });
}

void AnkiClient::showNextCard()
{
    if (m_study.cards.isEmpty()) {
        setCurrentState(QStringLiteral("DONE"));
        return;
    }

    const StudyCard &card = m_study.cards.first();

    m_currentFront = stripHtml(card.question);
    emit currentFrontChanged();

    // Anki's answer HTML typically contains front+back separated by
    // <hr id=answer> (or <hr id="answer"> etc).  Extract just the back.
    QString backText;
    {
        QString answerHtml = card.answer;

        // Try literal match first, then regex for quoted/spaced variants
        int hrIdx = answerHtml.indexOf(QLatin1String("<hr id=answer>"),
                                       0, Qt::CaseInsensitive);
        if (hrIdx < 0) {
            static QRegularExpression re(
                QStringLiteral("<hr[^>]+id\\s*=\\s*[\"']?answer[\"']?[^>]*>"),
                QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch m = re.match(answerHtml);
            if (m.hasMatch())
                hrIdx = m.capturedStart();
        }

        if (hrIdx >= 0) {
            int tagEnd = answerHtml.indexOf(QLatin1Char('>'), hrIdx);
            if (tagEnd >= 0)
                backText = stripHtml(answerHtml.mid(tagEnd + 1));
        }

        // Fallback: if no separator found or result is empty,
        // strip the front text from the beginning of the full answer.
        if (backText.isEmpty()) {
            QString fullBack = stripHtml(answerHtml);
            if (fullBack.startsWith(m_currentFront)) {
                backText = fullBack.mid(m_currentFront.length()).trimmed();
                // Remove leading "---" left over from the <hr> conversion
                if (backText.startsWith(QLatin1String("---")))
                    backText = backText.mid(3).trimmed();
            }
            // Last resort: use the full stripped answer as-is
            if (backText.isEmpty())
                backText = fullBack;
        }
    }
    m_currentBack = backText;
    emit currentBackChanged();

    m_currentButtonLabels = card.button_labels;
    emit currentButtonLabelsChanged();

    m_currentRemaining = m_study.totalDue();
    emit currentRemainingChanged();

    setCurrentState(QStringLiteral("STUDY"));
}

void AnkiClient::answerCard(int button)
{
    if (m_study.cards.isEmpty()) return;

    // Copy the card being answered (we're about to remove it from the list)
    StudyCard answeredCard = m_study.cards.first();
    int64_t nextCardId = (m_study.cards.size() > 1)
                         ? m_study.cards[1].card_id : 0;

    m_cardsReviewed++;
    emit cardsReviewedChanged();

    // If there's a preloaded next card, show it immediately from cache
    // instead of blocking on the server round-trip.
    bool optimistic = m_study.cards.size() > 1;
    if (optimistic) {
        m_study.cards.removeFirst();
        // Optimistically decrement remaining count (server will correct)
        if (m_currentRemaining > 0) {
            m_currentRemaining--;
            emit currentRemainingChanged();
        }
        showNextCard();
    } else {
        // Last card — must wait for server to know if there are more
        setCurrentState(QStringLiteral("LOADING"));
        setStatusMessage(QStringLiteral("Sending answer..."));
    }

    QByteArray body = encodeAnswerCardRequest(answeredCard, button, nextCardId);

    postStudy(QStringLiteral("/svc/study/study-cards"), body,
              [this, optimistic](bool ok, QByteArray data) {
        if (!ok) {
            if (!optimistic)
                setError(QString::fromUtf8(data));
            else
                qDebug() << "Background answer POST failed:" << data;
            return;
        }

        m_study = decodeStudyCardsResponse(data);
        cacheStudyCards();

        if (optimistic) {
            // We already showed the next card. Silently update remaining
            // count and scheduling data from the authoritative server response.
            m_currentRemaining = m_study.totalDue();
            emit currentRemainingChanged();

            if (!m_study.cards.isEmpty()) {
                const StudyCard &fresh = m_study.cards.first();
                m_currentButtonLabels = fresh.button_labels;
                emit currentButtonLabelsChanged();
            }
        } else {
            // We were in LOADING state, now show whatever came back
            if (m_study.cards.isEmpty()) {
                setCurrentState(QStringLiteral("DONE"));
            } else {
                showNextCard();
            }
        }
    });
}
