#pragma once

#include "Types.h"
#include "ProductType.h"
#include "TnmClient.h"
#include "TigerClient.h"
#include "MrlcClient.h"
#include "Atlas14Client.h"          // NEW
#include "TigerClient.h"
#include "MrlcClient.h"
#include <QString>
#include <QList>
#include <memory>

class CsvTable;
class QTextStream;

// Configuration for a processing run, populated from CLI options.
struct ProcessOptions {
    QString latColOverride;
    QString lonColOverride;
    QString idColOverride;
    double  bufferMeters = 30.0;
    QString downloadDir;            // empty => no download
    int     maxTiles     = -1;      // -1 => use each product's default
    bool    makePoints   = false;   // write a single-point shapefile per site
    QString pointsDir;              // base dir for point shapefiles
    bool    doAtlas14    = false;   // NEW: query NOAA Atlas 14 per site
};

// Walks each site in a CsvTable, resolves the best resolution for each
// requested ProductType (and optionally downloads), optionally queries
// Atlas 14, and writes an augmented output CSV.
class SiteProcessor {
public:
    SiteProcessor(TnmClient& client,
                  TigerClient& tiger,
                  MrlcClient& mrlc,
                  QList<std::shared_ptr<ProductType>> products,
                  ProcessOptions opts);

    // Returns false on a fatal setup error (missing columns, unwritable out).
    bool run(const CsvTable& table, const QString& outPath,
             QTextStream& log, QString* err);

private:
    TnmClient&   m_client;
    TigerClient& m_tiger;
    MrlcClient&  m_mrlc;
    Atlas14Client m_atlas14;        // NEW — owns its own QNetworkAccessManager
    QList<std::shared_ptr<ProductType>> m_products;
    ProcessOptions m_opts;

    // Resolve one TNM product type for one point (walk tiers, first hit wins).
    ProductOutcome resolveProduct(const ProductType& product,
                                  double lat, double lon,
                                  const QString& siteId,
                                  QTextStream& log);

    // Download winning tiles for one TNM product into <dir>/<siteId>/<key>/.
    // County-based products (roads) resolve + download via TIGER/Line in
    // one step (geocode -> county FIPS -> download). Returns the outcome
    // with status/url/downloaded filled in.
    ProductOutcome resolveAndDownloadRoads(const ProductType& product,
                                           double lat, double lon,
                                           const QString& siteId,
                                           QTextStream& log);

    // Bbox-raster products (NLCD land cover) resolve + download via a single
    // WCS GetCoverage clipped to the site's bounding box.
    ProductOutcome resolveAndDownloadLandCover(const ProductType& product,
                                               double lat, double lon,
                                               const QString& siteId,
                                               QTextStream& log);

    // Download the winning tiles for one product into <dir>/<siteId>/<key>/.
    void downloadOutcome(const ProductType& product, ProductOutcome& outcome,
                         const QString& siteId, QTextStream& log);

    // Query Atlas 14, and write atlas14_pf.csv under <download_dir>/<siteId>/atlas14/
    // when --download is set.  Mirrors the resolve-and-download pattern of other layers.
    Atlas14Result resolveAndDownloadAtlas14(double lat, double lon,
                                            const QString& siteId, QTextStream& log);

    // Write a single-point shapefile for one site.
    QString writePointShapefile(const CsvTable& table, int rowIdx,
                                const QString& siteId, double lon, double lat,
                                int latC, int lonC, QTextStream& log);

    static QString sanitize(const QString& s);
    static QString fileNameFromUrl(const QString& url);
};
