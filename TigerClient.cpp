#include "TigerClient.h"
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QTextStream>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

// Census geocoder: coordinates -> geographies (returns the county containing
// the point). vintage/benchmark are required; "Current_Current" tracks the
// latest published geography.
static const char* CENSUS_GEOCODER =
    "https://geocoding.geo.census.gov/geocoder/geographies/coordinates";
static const char* TIGER_BASE =
    "https://www2.census.gov/geo/tiger";

TigerClient::TigerClient(int year, int geocodeTimeoutMs, int downloadTimeoutMs)
    : m_year(year), m_geocodeTimeoutMs(geocodeTimeoutMs),
      m_downloadTimeoutMs(downloadTimeoutMs) {}

QByteArray TigerClient::getSync(const QString& url, int timeoutMs, QString* err) {
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, "demcheck/2.1 (EnviroInformatics)");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam.get(req);

    QEventLoop loop;
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    QByteArray body;
    if (!timer.isActive()) {
        if (err) *err = "timeout";
        reply->abort();
    } else {
        timer.stop();
        if (reply->error() != QNetworkReply::NoError) {
            if (err) *err = reply->errorString();
        } else {
            body = reply->readAll();
        }
    }
    reply->deleteLater();
    return body;
}

QString TigerClient::countyFipsForPoint(double lat, double lon,
                                        QString* countyName, QString* err) {
    QUrl url(CENSUS_GEOCODER);
    QUrlQuery q;
    q.addQueryItem("x", QString::number(lon, 'f', 8));   // x = longitude
    q.addQueryItem("y", QString::number(lat, 'f', 8));   // y = latitude
    q.addQueryItem("benchmark", "Public_AR_Current");
    q.addQueryItem("vintage", "Current_Current");
    q.addQueryItem("layers", "Counties");
    q.addQueryItem("format", "json");
    url.setQuery(q);

    QString gerr;
    const QByteArray body = getSync(url.toString(), m_geocodeTimeoutMs, &gerr);
    if (body.isEmpty()) {
        if (err) *err = "geocode failed: " + gerr;
        return QString();
    }

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = "geocode bad JSON: " + pe.errorString();
        return QString();
    }
    // result -> geographies -> Counties[0] -> { STATE, COUNTY, NAME, GEOID }
    const QJsonObject result = doc.object().value("result").toObject();
    const QJsonObject geos = result.value("geographies").toObject();
    const QJsonArray counties = geos.value("Counties").toArray();
    if (counties.isEmpty()) {
        if (err) *err = "no county found for point (offshore or outside US?)";
        return QString();
    }
    const QJsonObject c = counties.first().toObject();
    QString geoid = c.value("GEOID").toString();          // 5-digit state+county
    if (geoid.isEmpty()) {
        // fall back to STATE + COUNTY fields
        const QString st = c.value("STATE").toString();
        const QString co = c.value("COUNTY").toString();
        if (!st.isEmpty() && !co.isEmpty()) geoid = st + co;
    }
    if (countyName)
        *countyName = c.value("NAME").toString() + ", " +
                      c.value("STUSAB").toString();
    if (geoid.size() != 5) {
        if (err) *err = "unexpected county GEOID: '" + geoid + "'";
        return QString();
    }
    return geoid;
}

QString TigerClient::roadsUrlForFips(const QString& fips) const {
    // https://www2.census.gov/geo/tiger/TIGER<year>/ROADS/tl_<year>_<fips>_roads.zip
    return QString("%1/TIGER%2/ROADS/tl_%2_%3_roads.zip")
        .arg(TIGER_BASE).arg(m_year).arg(fips);
}

bool TigerClient::download(const QString& url, const QString& destPath,
                           qint64 expectedBytes, QTextStream* log) {
    QFileInfo fi(destPath);
    if (fi.exists() && fi.size() > 0 &&
        (expectedBytes < 0 || fi.size() == expectedBytes)) {
        if (log) { *log << "      exists, skipping: " << fi.fileName() << "\n"; log->flush(); }
        return true;
    }

    const QString partPath = destPath + ".part";
    QFile f(partPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (log) { *log << "      cannot write: " << partPath << "\n"; log->flush(); }
        return false;
    }

    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, "demcheck/2.1 (EnviroInformatics)");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam.get(req);

    QEventLoop loop;
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        f.write(reply->readAll());
        timer.start(m_downloadTimeoutMs);
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(m_downloadTimeoutMs);
    loop.exec();

    bool ok = false;
    if (!timer.isActive()) {
        if (log) { *log << "      timeout\n"; log->flush(); }
        reply->abort();
    } else {
        timer.stop();
        f.write(reply->readAll());
        // a 404 (county file missing) comes back as an HTTP error here
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (reply->error() != QNetworkReply::NoError) {
            if (log) { *log << "      error: " << reply->errorString()
                            << " (HTTP " << status.toInt() << ")\n"; log->flush(); }
        } else {
            ok = true;
        }
    }
    reply->deleteLater();
    f.close();

    if (ok && f.size() > 0) {
        if (QFile::exists(destPath)) QFile::remove(destPath);
        if (QFile::rename(partPath, destPath)) return true;
        if (log) { *log << "      cannot finalize: " << destPath << "\n"; log->flush(); }
        return false;
    }
    f.remove();
    return false;
}
