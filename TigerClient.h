#pragma once
#include <QString>
#include <QtNetwork/QNetworkAccessManager>

class QTextStream;

// Fetches Census TIGER/Line "All Roads" data, which (unlike 3DEP/NHD) is served
// as one Shapefile package PER COUNTY, not by bounding box. The USGS National
// Transportation Dataset is unusable through TNMAccess (the product query errors
// server-side), so roads come from the Census Bureau directly instead.
//
// Two steps per site:
//   1) geocode a (lat, lon) to a county FIPS via the Census geographies API
//   2) download tl_<year>_<FIPS>_roads.zip from the TIGER file directory
//
// Both are synchronous (local event loop), matching TnmClient's style so the
// caller can iterate sites in a simple loop.
class TigerClient {
public:
    explicit TigerClient(int year = 2025,
                         int geocodeTimeoutMs = 30000,
                         int downloadTimeoutMs = 300000);

    // Resolve a 5-digit county FIPS (state+county) for a point. Returns empty
    // on failure; *err is set with a reason. *countyName is the human label.
    QString countyFipsForPoint(double lat, double lon,
                               QString* countyName, QString* err);

    // Build the TIGER roads zip URL for a county FIPS.
    QString roadsUrlForFips(const QString& fips) const;

    // Download a URL to destPath (streamed, .part rename, skip-on-exist).
    bool download(const QString& url, const QString& destPath,
                  qint64 expectedBytes, QTextStream* log);

    int year() const { return m_year; }

private:
    QNetworkAccessManager m_nam;
    int m_year;
    int m_geocodeTimeoutMs;
    int m_downloadTimeoutMs;

    QByteArray getSync(const QString& url, int timeoutMs, QString* err);
};
