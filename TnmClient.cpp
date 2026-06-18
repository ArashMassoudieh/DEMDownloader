#include "TnmClient.h"
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
#include <QtCore/QtMath>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

static const char* TNM_PRODUCTS = "https://tnmaccess.nationalmap.gov/api/v1/products";

TnmClient::TnmClient(int queryTimeoutMs, int downloadTimeoutMs)
    : m_queryTimeoutMs(queryTimeoutMs), m_downloadTimeoutMs(downloadTimeoutMs) {}

QString TnmClient::buildBbox(double lat, double lon, double bufferMeters) {
    // ~111320 m per degree latitude; longitude scaled by cos(lat).
    double dLat = bufferMeters / 111320.0;
    double cosL = qCos(qDegreesToRadians(lat));
    if (qFabs(cosL) < 1e-6) cosL = 1e-6;
    double dLon = bufferMeters / (111320.0 * cosL);
    // bbox order: xmin,ymin,xmax,ymax  (lon,lat)
    return QString("%1,%2,%3,%4")
        .arg(lon - dLon, 0, 'f', 8).arg(lat - dLat, 0, 'f', 8)
        .arg(lon + dLon, 0, 'f', 8).arg(lat + dLat, 0, 'f', 8);
}

QueryResult TnmClient::query(double lat, double lon, double bufferMeters,
                             const QString& dataset, const QString& prodFormats) {
    QueryResult r;

    QUrl url(TNM_PRODUCTS);
    QUrlQuery q;
    q.addQueryItem("bbox", buildBbox(lat, lon, bufferMeters));
    q.addQueryItem("datasets", dataset);
    if (!prodFormats.isEmpty()) q.addQueryItem("prodFormats", prodFormats);
    q.addQueryItem("outputFormat", "JSON");
    q.addQueryItem("max", "50");
    q.addQueryItem("offset", "0");
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "demcheck/2.0 (EnviroInformatics)");
    QNetworkReply* reply = m_nam.get(req);

    QEventLoop loop;
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(m_queryTimeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        r.error = "timeout";
        reply->abort(); reply->deleteLater();
        return r;
    }
    timer.stop();

    if (reply->error() != QNetworkReply::NoError) {
        r.error = reply->errorString();
        reply->deleteLater();
        return r;
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = "bad JSON: " + pe.errorString();
        return r;
    }
    const QJsonObject obj = doc.object();
    r.tileCount = obj.value("total").toInt(0);
    r.ok = true;

    const QJsonArray items = obj.value("items").toArray();
    for (const QJsonValue& v : items) {
        const QJsonObject it = v.toObject();
        Tile tile;
        const QJsonObject urls = it.value("urls").toObject();
        // Elevation items expose urls.TIFF; vector products expose
        // urls.Shapefile / downloadURL. Probe the common keys in order.
        tile.url = urls.value("TIFF").toString();
        if (tile.url.isEmpty()) tile.url = urls.value("Shapefile").toString();
        if (tile.url.isEmpty()) tile.url = urls.value("GeoPackage").toString();
        if (tile.url.isEmpty()) tile.url = urls.value("FileGDB").toString();
        if (tile.url.isEmpty()) tile.url = it.value("downloadURL").toString();
        if (tile.url.isEmpty()) continue;

        tile.name = it.value("title").toString();
        const QJsonValue sz = it.value("sizeInBytes");
        if (sz.isDouble())      tile.bytes = static_cast<qint64>(sz.toDouble());
        else if (sz.isString()) tile.bytes = sz.toString().toLongLong();
        r.tiles.append(tile);
    }
    if (!items.isEmpty()) {
        const QJsonObject first = items.first().toObject();
        r.date = first.value("publicationDate").toString();
        if (r.date.isEmpty()) r.date = first.value("dateCreated").toString();
    }
    if (!r.tiles.isEmpty()) r.firstUrl = r.tiles.first().url;
    return r;
}

bool TnmClient::download(const QString& url, const QString& destPath,
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
    req.setHeader(QNetworkRequest::UserAgentHeader, "demcheck/2.0 (EnviroInformatics)");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam.get(req);

    QEventLoop loop;
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        f.write(reply->readAll());
        timer.start(m_downloadTimeoutMs); // reset inactivity timer per chunk
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
        if (reply->error() != QNetworkReply::NoError) {
            if (log) { *log << "      error: " << reply->errorString() << "\n"; log->flush(); }
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
