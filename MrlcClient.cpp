#include "MrlcClient.h"
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QTextStream>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <cmath>

// --- Service specifics ------------------------------------------------------
//
// USGS/MRLC production GeoServer, Annual NLCD Land Cover (CONUS, native).
// From https://www.mrlc.gov/data-services-page ("Annual NLCD" -> "Land Cover").
static const char* WCS_BASE =
    "https://dmsdata.cr.usgs.gov/geoserver/mrlc_Land-Cover-Native_conus_year_data/wcs";

// GeoServer coverage id. WCS 2.0.1 ids use a double underscore for the
// workspace:layer separator. The workspace is the same token as the path
// segment above; the layer is conventionally the same name. If a live
// GetCapabilities shows a different id, change ONLY this string.
//   curl "https://dmsdata.cr.usgs.gov/geoserver/mrlc_Land-Cover-Native_conus_year_data/wcs?service=WCS&version=2.0.1&request=GetCapabilities" | grep -i CoverageId
static const char* WCS_COVERAGE_ID =
    "mrlc_Land-Cover-Native_conus_year_data:Land-Cover-Native_conus_year_data";

// Native CRS axis order for this coverage is geographic lat/lon (EPSG:4326).
// GeoServer 2.0.1 subsets are by axis label; for 4326 coverages the axis
// labels are typically "Lat"/"Long" (note GeoServer's "Long", not "Lon").
// If a probe shows "X"/"Y" or "i"/"j", change these two.
static const char* AXIS_X = "X";
static const char* AXIS_Y = "Y";

// Time axis: Annual NLCD is published one slice per year. WCS exposes this as a
// temporal subset. GeoServer commonly labels it "time" and expects an ISO
// instant; the year is mapped to <year>-01-01T00:00:00Z. If the coverage uses
// a custom dimension name, change TIME_AXIS.
static const char* TIME_AXIS = "time";
// ---------------------------------------------------------------------------

MrlcClient::MrlcClient(int year, int downloadTimeoutMs)
    : m_year(year), m_downloadTimeoutMs(downloadTimeoutMs) {}

void MrlcClient::bboxDegrees(double lat, double lon, double bufferMeters,
                             double* minLon, double* minLat,
                             double* maxLon, double* maxLat) {
    // ~111,320 m per degree of latitude; longitude shrinks by cos(lat).
    const double mPerDegLat = 111320.0;
    const double latRad = lat * M_PI / 180.0;
    double mPerDegLon = mPerDegLat * std::cos(latRad);
    if (mPerDegLon < 1.0) mPerDegLon = 1.0;            // guard near the poles
    const double dLat = bufferMeters / mPerDegLat;
    const double dLon = bufferMeters / mPerDegLon;
    *minLat = lat - dLat; *maxLat = lat + dLat;
    *minLon = lon - dLon; *maxLon = lon + dLon;
}

QString MrlcClient::coverageUrlForBbox(double lat, double lon,
                                       double bufferMeters) const {
    double minLon, minLat, maxLon, maxLat;
    bboxDegrees(lat, lon, bufferMeters, &minLon, &minLat, &maxLon, &maxLat);

    QUrl url(WCS_BASE);
    QUrlQuery q;
    q.addQueryItem("service", "WCS");
    q.addQueryItem("version", "2.0.1");
    q.addQueryItem("request", "GetCoverage");
    q.addQueryItem("coverageId", WCS_COVERAGE_ID);
    q.addQueryItem("format", "image/tiff");
    // Spatial subsets, one per axis: subset=Long(min,max)&subset=Lat(min,max).
    q.addQueryItem("subset",
        QString("%1(%2,%3)").arg(AXIS_X)
            .arg(minLon, 0, 'f', 8).arg(maxLon, 0, 'f', 8));
    q.addQueryItem("subset",
        QString("%1(%2,%3)").arg(AXIS_Y)
            .arg(minLat, 0, 'f', 8).arg(maxLat, 0, 'f', 8));
    // Tell the server our subset is in lon/lat degrees regardless of the
    // coverage's native (projected) CRS, and ask the output back in 4326.
    q.addQueryItem("subsettingCrs", "http://www.opengis.net/def/crs/EPSG/0/4326");
    q.addQueryItem("outputCrs",     "http://www.opengis.net/def/crs/EPSG/0/4326");
    // Temporal subset: pin to the requested Annual NLCD year.
    q.addQueryItem("subset",
        QString("%1(\"%2-01-01T00:00:00Z\")").arg(TIME_AXIS).arg(m_year));

    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded);
}

bool MrlcClient::download(const QString& url, const QString& destPath,
                          QTextStream* log) {
    QFileInfo fi(destPath);
    if (fi.exists() && fi.size() > 0) {
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
    req.setHeader(QNetworkRequest::UserAgentHeader, "demcheck/2.2 (EnviroInformatics)");
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
    QString contentType;
    if (!timer.isActive()) {
        if (log) { *log << "      timeout\n"; log->flush(); }
        reply->abort();
    } else {
        timer.stop();
        f.write(reply->readAll());
        contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (reply->error() != QNetworkReply::NoError) {
            const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            if (log) { *log << "      error: " << reply->errorString()
                            << " (HTTP " << status.toInt() << ")\n"; log->flush(); }
        } else {
            ok = true;
        }
    }
    reply->deleteLater();
    f.close();

    // GeoServer returns a 200 with an XML ServiceExceptionReport when the
    // request is malformed (bad coverage id, wrong axis label, out-of-range
    // time). Sniff the first bytes: a real GeoTIFF starts with "II*\0" (little
    // endian) or "MM\0*" (big endian). Anything starting with '<' is an error.
    if (ok) {
        QFile chk(partPath);
        if (chk.open(QIODevice::ReadOnly)) {
            const QByteArray head = chk.read(4);
            chk.close();
            const bool isTiff =
                head.startsWith(QByteArray("II*\x00", 4)) ||
                head.startsWith(QByteArray("MM\x00*", 4));
            const bool looksXml =
                head.startsWith('<') ||
                contentType.contains("xml", Qt::CaseInsensitive) ||
                contentType.contains("ogc", Qt::CaseInsensitive);
            if (!isTiff || looksXml) {
                if (log) {
                    *log << "      server returned a non-GeoTIFF body";
                    if (!contentType.isEmpty()) *log << " (content-type: " << contentType << ")";
                    *log << "; treating as error. First bytes: "
                         << QString::fromLatin1(head.toHex(' ')) << "\n";
                    log->flush();
                }
                ok = false;
            }
        }
    }

    if (ok && f.size() > 0) {
        if (QFile::exists(destPath)) QFile::remove(destPath);
        if (QFile::rename(partPath, destPath)) return true;
        if (log) { *log << "      cannot finalize: " << destPath << "\n"; log->flush(); }
        return false;
    }
    QFile::remove(partPath);
    return false;
}
