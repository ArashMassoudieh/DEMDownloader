#pragma once

#include <QString>
#include <QMap>
#include <QStringList>
#include <QtNetwork/QNetworkAccessManager>

// Precipitation depth table keyed by duration label -> return period label -> depth (inches).
// Duration labels match HDSC keys: "60min", "06hr", "24hr", etc.
// Return period labels: "2yr", "5yr", "10yr", "25yr", "50yr", "100yr".
using PfTable = QMap<QString, QMap<QString, double>>;

// Result of one Atlas 14 point query.
struct Atlas14Result {
    bool    ok         = false;
    PfTable table;               // full depth table (partial-duration series, inches)
    QString error;               // populated when ok == false

    // Populated by SiteProcessor::resolveAndDownloadAtlas14() when --download is set.
    int     downloaded = 0;      // 1 if atlas14_pf.csv was written, 0 otherwise
    QString downloadDir;         // absolute path to <siteId>/atlas14/ folder

    // Convenience accessors for the two design storm durations.
    // Returns -1.0 if the key is not present.
    double depth(const QString& duration, const QString& returnPeriod) const;
    // HDSC has returned both "06hr" and "6hr" depending on server version;
    // try both so lookups are robust to server-side formatting changes.
    double depth6hr100yr() const {
        double v = depth("06hr", "100yr");
        return (v >= 0.0) ? v : depth("6hr", "100yr");
    }
    double depth24hr100yr() const {
        double v = depth("24hr", "100yr");
        return (v >= 0.0) ? v : depth("24hr", "100yr");  // same key both ways
    }
};

// Queries NOAA HDSC REST API (Atlas 14) for precipitation frequency estimates
// at a point location.  All requests are synchronous (local QEventLoop) so
// the caller can iterate sites in a simple loop, matching TnmClient's pattern.
//
// Endpoint used:
//   https://hdsc.nws.noaa.gov/cgi-bin/hdsc/new/cgi_readH5.py
//     ?aoi=point&lat=<lat>&lon=<lon>&type=pf&data=depth&units=english&series=pd
//
// The response JSON has the shape:
//   { "data": { "<duration>": { "<freq>": [estimate, lower, upper], ... }, ... },
//     "freq":  ["2",  "5",  "10",  "25",  "50",  "100"],
//     "duration": ["5min","10min","15min","30min","60min","2hr","3hr",
//                  "6hr","12hr","24hr","2day","3day","4day","7day",
//                  "10day","20day","30day","45day","60day"] }
//
// We store all durations but expose convenience accessors for 6hr and 24hr.

class Atlas14Client {
public:
    explicit Atlas14Client(int timeoutMs = 30000);

    // Query Atlas 14 at (lat, lon).  lat/lon in decimal degrees (WGS84).
    Atlas14Result query(double lat, double lon);

private:
    QNetworkAccessManager m_nam;
    int m_timeoutMs;

    static QString buildUrl(double lat, double lon);
};
