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

// WCS 1.0.0, deliberately, not 2.0.1. The server's 2.0.1 GetCoverage fails on
// this coverage with an internal NPE ("Failed to read the coverage ... Cannot
// invoke Object.getClass() because startTime is null") for every parameter
// combination, including a request with no time subset at all. 1.0.0 serves the
// same data correctly. Note the two versions spell the coverage id differently:
// 1.0.0 uses workspace:layer, 2.0.1 uses workspace__layer.
static const char* WCS_VERSION = "1.0.0";

// GeoServer coverage id, workspace:layer. The workspace is the same token as
// the path segment above. If a live GetCapabilities shows a different id,
// change ONLY this string.
//   curl "https://dmsdata.cr.usgs.gov/geoserver/mrlc_Land-Cover-Native_conus_year_data/wcs?service=WCS&version=1.0.0&request=GetCapabilities" | grep -i '<name>'
static const char* WCS_COVERAGE_ID =
    "mrlc_Land-Cover-Native_conus_year_data:Land-Cover-Native_conus_year_data";

// The coverage's native CRS. Requesting the bbox in EPSG:4326 instead makes
// GeoServer warp the Albers grid, which both resamples the categorical class
// codes and leaves the rotated grid's corners outside the returned box (~27%
// NoData at CONUS mid-longitudes). Asking in the native CRS avoids both.
static const char* NLCD_CRS = "EPSG:5070";

// NLCD grid: 30 m pixels. The CONUS coverage origin is (-2415585, 164805), so
// native pixel edges fall on x,y == 15 (mod 30). Snapping the request box to
// that lattice makes GeoServer hand back the source pixels untouched.
static const double NLCD_PIXEL_M  = 30.0;
static const double NLCD_GRID_OFF = 15.0;

// EPSG:5070 Albers Equal Area Conic (CONUS), on GRS80.
static const double ALB_A       = 6378137.0;          // GRS80 semi-major axis
static const double ALB_E2      = 0.00669438002290;   // GRS80 first eccentricity^2
static const double ALB_LAT_1   = 29.5;               // 1st standard parallel
static const double ALB_LAT_2   = 45.5;               // 2nd standard parallel
static const double ALB_LAT_0   = 23.0;               // latitude of origin
static const double ALB_LON_0   = -96.0;              // central meridian
// ---------------------------------------------------------------------------

namespace {

inline double deg2rad(double d) { return d * M_PI / 180.0; }

// Snyder eq. 3-12: authalic-area term q(phi).
double albersQ(double sinPhi) {
    const double e  = std::sqrt(ALB_E2);
    const double es = ALB_E2 * sinPhi * sinPhi;
    return (1.0 - ALB_E2) *
           (sinPhi / (1.0 - es) -
            (1.0 / (2.0 * e)) * std::log((1.0 - e * sinPhi) / (1.0 + e * sinPhi)));
}

// Snyder eq. 14-15: m(phi).
double albersM(double sinPhi, double cosPhi) {
    return cosPhi / std::sqrt(1.0 - ALB_E2 * sinPhi * sinPhi);
}

// Round v outward to the NLCD grid lattice (x == NLCD_GRID_OFF mod NLCD_PIXEL_M).
double snapToGrid(double v, bool up) {
    const double k = (v - NLCD_GRID_OFF) / NLCD_PIXEL_M;
    const double n = up ? std::ceil(k) : std::floor(k);
    return n * NLCD_PIXEL_M + NLCD_GRID_OFF;
}

} // namespace

void MrlcClient::lonLatToAlbers(double lon, double lat, double* x, double* y) {
    const double p1 = deg2rad(ALB_LAT_1), p2 = deg2rad(ALB_LAT_2);
    const double p0 = deg2rad(ALB_LAT_0), l0 = deg2rad(ALB_LON_0);
    const double p  = deg2rad(lat),       l  = deg2rad(lon);

    const double m1 = albersM(std::sin(p1), std::cos(p1));
    const double m2 = albersM(std::sin(p2), std::cos(p2));
    const double q1 = albersQ(std::sin(p1));
    const double q2 = albersQ(std::sin(p2));
    const double q0 = albersQ(std::sin(p0));
    const double q  = albersQ(std::sin(p));

    const double n    = (m1 * m1 - m2 * m2) / (q2 - q1);
    const double C    = m1 * m1 + n * q1;
    const double rho  = ALB_A * std::sqrt(C - n * q)  / n;
    const double rho0 = ALB_A * std::sqrt(C - n * q0) / n;
    const double th   = n * (l - l0);

    *x = rho * std::sin(th);
    *y = rho0 - rho * std::cos(th);
}

MrlcClient::MrlcClient(int year, int downloadTimeoutMs)
    : m_year(year), m_downloadTimeoutMs(downloadTimeoutMs) {}

void MrlcClient::bboxAlbers(double lat, double lon, double bufferMeters,
                            double* minX, double* minY,
                            double* maxX, double* maxY) {
    // The buffer is already in meters and Albers is an equal-area projection in
    // meters, so the box is built directly around the projected point -- no
    // degrees-per-meter approximation anywhere.
    double cx, cy;
    lonLatToAlbers(lon, lat, &cx, &cy);
    *minX = snapToGrid(cx - bufferMeters, false);
    *minY = snapToGrid(cy - bufferMeters, false);
    *maxX = snapToGrid(cx + bufferMeters, true);
    *maxY = snapToGrid(cy + bufferMeters, true);
}

QString MrlcClient::coverageUrlForBbox(double lat, double lon,
                                       double bufferMeters) const {
    double minX, minY, maxX, maxY;
    bboxAlbers(lat, lon, bufferMeters, &minX, &minY, &maxX, &maxY);

    QUrl url(WCS_BASE);
    QUrlQuery q;
    q.addQueryItem("service", "WCS");
    q.addQueryItem("version", WCS_VERSION);
    q.addQueryItem("request", "GetCoverage");
    q.addQueryItem("coverage", WCS_COVERAGE_ID);
    q.addQueryItem("format", "GeoTIFF");
    // WCS 1.0.0 takes one bbox in the CRS named by `crs`, plus an explicit
    // output resolution. Both are the coverage's native values, so GeoServer
    // returns source pixels rather than a warp.
    q.addQueryItem("crs", NLCD_CRS);
    q.addQueryItem("bbox", QString("%1,%2,%3,%4")
        .arg(minX, 0, 'f', 3).arg(minY, 0, 'f', 3)
        .arg(maxX, 0, 'f', 3).arg(maxY, 0, 'f', 3));
    q.addQueryItem("resx", QString::number(NLCD_PIXEL_M, 'f', 1));
    q.addQueryItem("resy", QString::number(NLCD_PIXEL_M, 'f', 1));
    // Annual NLCD publishes one slice per year; pin to the requested epoch.
    // The slice instants are midnight UTC on Jan 1, with milliseconds.
    q.addQueryItem("time", QString("%1-01-01T00:00:00.000Z").arg(m_year));

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
