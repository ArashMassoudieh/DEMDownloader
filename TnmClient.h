#pragma once
#include "Types.h"
#include <QString>
#include <QtNetwork/QNetworkAccessManager>

class QTextStream;

// Owns the network manager and performs all TNMAccess HTTP work:
//  - querying a single dataset (tier) within a bounding box around a point
//  - downloading a file to disk (streamed, with .part rename and skip-on-exist)
//
// All requests are synchronous (driven by a local event loop) so the caller
// can iterate sites in a simple loop.
class TnmClient {
public:
    explicit TnmClient(int queryTimeoutMs = 30000, int downloadTimeoutMs = 120000);

    // Query one dataset at (lat, lon) using a bbox of half-width bufferMeters.
    QueryResult query(double lat, double lon, double bufferMeters,
                      const QString& dataset, const QString& prodFormats);

    // Download one file to destPath (streamed via destPath + ".part").
    // Skips if destPath already exists at expectedBytes (or any size if -1).
    // Follows redirects. Logs progress lines to `log` if non-null.
    bool download(const QString& url, const QString& destPath,
                  qint64 expectedBytes, QTextStream* log);

private:
    QNetworkAccessManager m_nam;
    int m_queryTimeoutMs;
    int m_downloadTimeoutMs;

    static QString buildBbox(double lat, double lon, double bufferMeters);
};
