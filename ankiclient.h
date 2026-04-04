#ifndef ANKICLIENT_H
#define ANKICLIENT_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QHash>
#include <QSet>

// ---------------------------------------------------------------------------
// Data structs
// ---------------------------------------------------------------------------

struct DeckInfo {
    QString title;
    int64_t deck_id;
    int newC;
    int learnC;
    int dueC;
    int indent;
    bool hasChildren;
    int totalDue;
};

struct StudyCard {
    int64_t card_id;
    QString question;
    QString answer;
    uint32_t count_index;
    QStringList button_labels;
    int64_t note_id;
    uint32_t template_index;

    // Raw NextCardStates sub-message bytes for passthrough
    QByteArray nextStates_current;
    QByteArray nextStates_again;
    QByteArray nextStates_hard;
    QByteArray nextStates_good;
    QByteArray nextStates_easy;
};

struct StudyResponse {
    uint32_t sched_ver;
    QList<StudyCard> cards;
    uint32_t new_count;
    uint32_t learn_count;
    uint32_t review_count;

    int totalDue() const {
        return static_cast<int>(new_count + learn_count + review_count);
    }
};

// ---------------------------------------------------------------------------
// AnkiClient — the C++ backend exposed to QML
// ---------------------------------------------------------------------------

class AnkiClient : public QObject
{
    Q_OBJECT

    // Properties bound by QML
    Q_PROPERTY(QString currentState    READ currentState    NOTIFY currentStateChanged)
    Q_PROPERTY(QVariantList deckData   READ deckData        NOTIFY deckDataChanged)
    Q_PROPERTY(QString currentDeckName READ currentDeckName NOTIFY currentDeckNameChanged)
    Q_PROPERTY(QString currentFront    READ currentFront    NOTIFY currentFrontChanged)
    Q_PROPERTY(QString currentBack     READ currentBack     NOTIFY currentBackChanged)
    Q_PROPERTY(QStringList currentButtonLabels READ currentButtonLabels NOTIFY currentButtonLabelsChanged)
    Q_PROPERTY(int currentRemaining    READ currentRemaining NOTIFY currentRemainingChanged)
    Q_PROPERTY(int currentTotal        READ currentTotal    NOTIFY currentTotalChanged)
    Q_PROPERTY(int cardsReviewed       READ cardsReviewed   NOTIFY cardsReviewedChanged)
    Q_PROPERTY(QString statusMessage   READ statusMessage   NOTIFY statusMessageChanged)
    Q_PROPERTY(QString errorMessage    READ errorMessage    NOTIFY errorMessageChanged)

public:
    explicit AnkiClient(QObject *parent = nullptr);

    // Property accessors
    QString currentState() const { return m_currentState; }
    QVariantList deckData() const { return m_deckData; }
    QString currentDeckName() const { return m_currentDeckName; }
    QString currentFront() const { return m_currentFront; }
    QString currentBack() const { return m_currentBack; }
    QStringList currentButtonLabels() const { return m_currentButtonLabels; }
    int currentRemaining() const { return m_currentRemaining; }
    int currentTotal() const { return m_currentTotal; }
    int cardsReviewed() const { return m_cardsReviewed; }
    QString statusMessage() const { return m_statusMessage; }
    QString errorMessage() const { return m_errorMessage; }

    // Q_INVOKABLEs called from QML
    Q_INVOKABLE void login(const QString &email, const QString &password);
    Q_INVOKABLE void loadDecks();
    Q_INVOKABLE void startStudy(int index);
    Q_INVOKABLE void answerCard(int button);
    Q_INVOKABLE void toggleDeck(int index);

signals:
    void currentStateChanged();
    void deckDataChanged();
    void currentDeckNameChanged();
    void currentFrontChanged();
    void currentBackChanged();
    void currentButtonLabelsChanged();
    void currentRemainingChanged();
    void currentTotalChanged();
    void cardsReviewedChanged();
    void statusMessageChanged();
    void errorMessageChanged();

private:
    // State setters (emit signals)
    void setCurrentState(const QString &s);
    void setStatusMessage(const QString &s);
    void setErrorMessage(const QString &s);
    void setError(const QString &msg);

    // Internal logic
    void showNextCard();
    void cacheStudyCards();  // cache content & fill stubs from cache
    void rebuildDeckData();  // rebuild QVariantList from m_decks + m_collapsedIndices

    // Auth helpers
    void handleAuthFailure();                      // clear cookies, go to LOGIN
    static QByteArray extractSetCookie(QNetworkReply *reply); // get "ankiweb=..." from Set-Cookie
    void saveToken(const QString &filename, const QByteArray &cookie);
    QByteArray loadToken(const QString &filename);
    void completeAnkiuserLogin(const QString &jwtToken); // step 3-4 of auth flow

    // Networking
    void postDecks(const QString &path, const QByteArray &body,
                   std::function<void(bool ok, QByteArray data)> callback);
    void postStudy(const QString &path, const QByteArray &body,
                   std::function<void(bool ok, QByteArray data)> callback);
    void post(const QString &url, const QByteArray &body,
              const QByteArray &cookie, const QByteArray &origin,
              const QByteArray &referer,
              std::function<void(bool ok, QByteArray data)> callback);

    // Network manager
    QNetworkAccessManager m_nam;

    // Backing data
    QString      m_currentState;
    QVariantList m_deckData;
    QString      m_currentDeckName;
    QString      m_currentFront;
    QString      m_currentBack;
    QStringList  m_currentButtonLabels;
    int          m_currentRemaining = 0;
    int          m_currentTotal     = 0;
    int          m_cardsReviewed    = 0;
    QString      m_statusMessage;
    QString      m_errorMessage;

    // Internal deck list (parallel to m_deckData, preserves int64 deck_id)
    QList<DeckInfo> m_decks;

    // Current study session
    StudyResponse m_study;

    // Card content cache — server omits question/answer for cards
    // whose content was already sent in a previous response
    QHash<int64_t, StudyCard> m_cardCache;

    // Deck collapse state — indices of collapsed parent decks
    QSet<int> m_collapsedIndices;

    // Session cookies (set after login)
    QByteArray m_ankiwebCookie;
    QByteArray m_ankiuserCookie;
};

#endif // ANKICLIENT_H
