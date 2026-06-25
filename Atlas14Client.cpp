#include "Atlas14Client.h"

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QDebug>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

static const char* HDSC_BASE =
    "https://hdsc.nws.noaa.gov/cgi-bin/hdsc/new/cgi_readH5.py";

// ---------------------------------------------------------------------------
// Atlas14Result helpers
// ---------------------------------------------------------------------------

double Atlas14Result::depth(const QString& duration,
                            const QString& returnPeriod) const
{
    auto dit = table.find(duration);
    if (dit == table.end()) return -1.0;
    auto fit = dit->find(returnPeriod);
    if (fit == dit->end()) return -1.0;
    return *fit;
}

// ---------------------------------------------------------------------------
// Atlas14Client
// ---------------------------------------------------------------------------

Atlas14Client::Atlas14Client(int timeoutMs)
    : m_timeoutMs(timeoutMs) {}

QString Atlas14Client::buildUrl(double lat, double lon)
{
    QUrl url(HDSC_BASE);
    QUrlQuery q;
    q.addQueryItem("aoi",    "point");
    q.addQueryItem("lat",    QString::number(lat,  'f', 6));
    q.addQueryItem("lon",    QString::number(lon,  'f', 6));
    q.addQueryItem("type",   "pf");
    q.addQueryItem("data",   "depth");
    q.addQueryItem("units",  "english");
    q.addQueryItem("series", "pd");      // partial-duration series
    url.setQuery(q);
    return url.toString();
}

// ---------------------------------------------------------------------------
// parseJsArray — extract all quoted string values from a JS array literal.
//
// Handles both flat arrays:   ['a', 'b', 'c']
// and nested (2D) arrays:     [['a','b'], ['c','d']]
//
// Returns a flat list of all quoted string values found, in order.
// ---------------------------------------------------------------------------
static QStringList parseJsArray(const QString& src, int start, int* endPos)
{
    QStringList values;
    int depth = 0;
    int i = start;
    const int n = src.length();

    while (i < n) {
        QChar c = src[i];
        if (c == '[') {
            depth++;
            i++;
        } else if (c == ']') {
            depth--;
            i++;
            if (depth == 0) break;
        } else if (c == '\'') {
            // Quoted string value — read until closing quote.
            int j = i + 1;
            while (j < n && src[j] != '\'') j++;
            values << src.mid(i + 1, j - i - 1);
            i = j + 1;
        } else {
            i++;
        }
    }

    if (endPos) *endPos = i;
    return values;
}

// ---------------------------------------------------------------------------
// extractJsVar — find   varName = <array literal>;   in src and return
// the flat string list of all quoted values inside the array.
// ---------------------------------------------------------------------------
static QStringList extractJsVar(const QString& src, const QString& varName)
{
    // Match:  varName = [
    const QString needle = varName + " = [";
    int idx = src.indexOf(needle);
    if (idx < 0) return {};
    int arrayStart = idx + varName.length() + 3;   // points at '['
    return parseJsArray(src, arrayStart, nullptr);
}

// ---------------------------------------------------------------------------
// HDSC response format (legacy JavaScript, NOT JSON):
//
//   result = 'values';
//   quantiles = [                    // 2D: [return_period_idx][duration_idx]
//     ['0.150','0.228',...],          // row 0 = 2-yr  (10 durations per row)
//     ['0.194','0.295',...],          // row 1 = 5-yr
//     ...                            // rows: 2,5,10,25,50,100,200,500,1000 yr
//   ];
//   upper = [...];                   // upper confidence bound (same shape)
//   lower = [...];                   // lower confidence bound (same shape)
//
// Duration order (19 standard Atlas 14 durations, innermost axis):
//   5min,10min,15min,30min,60min,2hr,3hr,6hr,12hr,24hr,
//   2day,3day,4day,7day,10day,20day,30day,45day,60day
//
// Return period order (10 values, outermost axis):
//   2yr,5yr,10yr,25yr,50yr,100yr,200yr,500yr,1000yr
//   (We only store the first 6 for flood design work.)
//
// All values are quoted strings in the JS array (e.g. '0.553').
// ---------------------------------------------------------------------------

// Canonical duration labels, in the order HDSC returns them.
static const QStringList HDSC_DURATIONS = {
    "5min","10min","15min","30min","60min",
    "2hr","3hr","6hr","12hr","24hr",
    "2day","3day","4day","7day","10day","20day","30day","45day","60day"
};

// Return period labels. HDSC partial-duration series standard order.
// We store up to these labels; actual count detected from response size.
static const QStringList HDSC_RETURN_PERIODS = {
    "2yr","5yr","10yr","25yr","50yr","100yr","200yr","500yr","1000yr"
};

Atlas14Result Atlas14Client::query(double lat, double lon)
{
    Atlas14Result r;

    QNetworkRequest req(QUrl(buildUrl(lat, lon)));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "demcheck/2.0 (EnviroInformatics)");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_nam.get(req);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer,  &QTimer::timeout,         &loop, &QEventLoop::quit);
    QObject::connect(reply,   &QNetworkReply::finished,  &loop, &QEventLoop::quit);
    timer.start(m_timeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        r.error = "timeout";
        reply->abort();
        reply->deleteLater();
        return r;
    }
    timer.stop();

    if (reply->error() != QNetworkReply::NoError) {
        r.error = reply->errorString();
        reply->deleteLater();
        return r;
    }

    const QString body = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    // -----------------------------------------------------------------------
    // Parse the JavaScript response.
    //
    // Extract the flat value list from the `quantiles` 2D array.
    // The array is row-major: row = return period, col = duration.
    // Flat index = rp_idx * nDurations + dur_idx.
    // -----------------------------------------------------------------------

    const QStringList values = extractJsVar(body, "quantiles");

    if (values.isEmpty()) {
        const QString snippet = body.left(300).replace('\n', ' ').replace('\r', ' ');
        r.error = "could not parse HDSC response | raw: " + snippet;
        return r;
    }

    const int nDur = HDSC_DURATIONS.size();       // 19
    const int nRp  = HDSC_RETURN_PERIODS.size();  // 9 labelled

    // Auto-detect actual return periods per duration row.
    // HDSC may send more columns than our label list (e.g. 10 instead of 9).
    // Using the wrong stride causes a diagonal corruption in the output table.
    const int nActualRp = (values.size() >= nDur && nDur > 0)
                              ? (values.size() / nDur)
                              : nRp;
    // This will appear in the DEMDownloader console output for verification.
    // Expected: nActualRp == 10 for HDSC partial-duration series.
    qDebug() << "Atlas14: flat values=" << values.size()
             << " nDur=" << nDur << " nActualRp=" << nActualRp;

    for (int dIdx = 0; dIdx < nDur; ++dIdx) {
        const QString& dur = HDSC_DURATIONS[dIdx];
        for (int rpIdx = 0; rpIdx < nRp; ++rpIdx) {
            const int flat = dIdx * nActualRp + rpIdx;
            if (flat >= values.size()) break;
            bool ok = false;
            double v = values[flat].toDouble(&ok);
            if (!ok) continue;
            r.table[dur][HDSC_RETURN_PERIODS[rpIdx]] = v;
        }
    }

    if (r.table.isEmpty()) {
        r.error = "quantiles array found but no numeric values extracted";
        return r;
    }

    r.ok = true;
    return r;
}
