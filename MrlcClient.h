#pragma once
#include <QString>
#include <QtNetwork/QNetworkAccessManager>

class QTextStream;

// Fetches NLCD (Annual NLCD, Collection 1) Land Cover from the USGS/MRLC
// production GeoServer WCS. Unlike 3DEP/NHD (TNMAccess, by bbox tiles) and
// roads (Census TIGER, by county), NLCD Land Cover is a single national
// coverage served via OGC Web Coverage Service: one GetCoverage request with a
// bounding-box subset returns an already-clipped GeoTIFF. No tier walking, no
// county geocode, one file per site.
//
// Endpoint (from https://www.mrlc.gov/data-services-page, "Annual NLCD" ->
// "Land Cover" WCS):
//   https://dmsdata.cr.usgs.gov/geoserver/mrlc_Land-Cover-Native_conus_year_data/wcs
//
// This is the CONUS coverage (covers AZ and NM). It is time-enabled: Annual
// NLCD publishes one layer per year, selected with the WCS time parameter. The
// coverage id is a GeoServer specific that can drift, so it is a constant near
// the top of MrlcClient.cpp and is easy to correct after a one-line probe (see
// the curl in the .cpp header comment).
//
// Requests use WCS 1.0.0, not 2.0.1: the server's 2.0.1 GetCoverage throws an
// internal NPE on this coverage regardless of parameters. See MrlcClient.cpp.
//
// Synchronous (local event loop), matching TnmClient / TigerClient so the
// caller can iterate sites in a simple loop.
class MrlcClient {
public:
    // year selects the Annual NLCD epoch (e.g. 2023). timeout for the export.
    explicit MrlcClient(int year = 2023, int downloadTimeoutMs = 300000);

    // Build the WCS GetCoverage URL for a square box of half-width bufferMeters
    // around a WGS84 point. The box is expressed in the coverage's native CRS
    // (EPSG:5070 Albers) so the server returns the untouched 30 m NLCD grid.
    // Returns the full request URL.
    QString coverageUrlForBbox(double lat, double lon, double bufferMeters) const;

    // Download the clipped GeoTIFF to destPath (streamed, .part rename,
    // skip-on-exist). GeoServer returns an XML ServiceExceptionReport (not a
    // GeoTIFF) on a bad request, so the body is sniffed and an XML/error body
    // is treated as failure rather than written as a .tif. Returns true on ok.
    bool download(const QString& url, const QString& destPath,
                  QTextStream* log);

    int year() const { return m_year; }

private:
    QNetworkAccessManager m_nam;
    int m_year;
    int m_downloadTimeoutMs;

    // WGS84 lon/lat -> EPSG:5070 Albers Equal Area (CONUS) easting/northing.
    // Snyder's forward formulas; the project links only Qt, so no PROJ.
    static void lonLatToAlbers(double lon, double lat, double* x, double* y);

    // Square box of half-width bufferMeters around the point, in Albers meters,
    // snapped outward to the NLCD 30 m grid so no resampling is needed.
    static void bboxAlbers(double lat, double lon, double bufferMeters,
                           double* minX, double* minY,
                           double* maxX, double* maxY);
};
