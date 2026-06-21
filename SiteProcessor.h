#pragma once
#include "Types.h"
#include "ProductType.h"
#include "TnmClient.h"
#include "TigerClient.h"
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
    double bufferMeters = 30.0;
    QString downloadDir;          // empty => no download
    int maxTiles = -1;            // -1 => use each product's default
    bool makePoints = false;      // write a single-point shapefile per site
    QString pointsDir;            // base dir for point shapefiles (fallback when no downloadDir)
};

// Walks each site in a CsvTable, resolves the best resolution for each
// requested ProductType (and optionally downloads), and writes an augmented
// output CSV. Holds no global state; one instance per run.
class SiteProcessor {
public:
    SiteProcessor(TnmClient& client,
                  TigerClient& tiger,
                  QList<std::shared_ptr<ProductType>> products,
                  ProcessOptions opts);

    // Returns false on a fatal setup error (missing columns, unwritable out).
    bool run(const CsvTable& table, const QString& outPath,
             QTextStream& log, QString* err);

private:
    TnmClient& m_client;
    TigerClient& m_tiger;
    QList<std::shared_ptr<ProductType>> m_products;
    ProcessOptions m_opts;

    // Resolve one product type for one point (walk tiers, first hit wins).
    ProductOutcome resolveProduct(const ProductType& product,
                                  double lat, double lon,
                                  const QString& siteId,
                                  QTextStream& log);

    // County-based products (roads) resolve + download via TIGER/Line in
    // one step (geocode -> county FIPS -> download). Returns the outcome
    // with status/url/downloaded filled in.
    ProductOutcome resolveAndDownloadRoads(const ProductType& product,
                                           double lat, double lon,
                                           const QString& siteId,
                                           QTextStream& log);

    // Download the winning tiles for one product into <dir>/<siteId>/<key>/.
    void downloadOutcome(const ProductType& product, ProductOutcome& outcome,
                         const QString& siteId, QTextStream& log);

    // Write a single-point shapefile for one site. Returns the directory used,
    // or empty on failure (error logged).
    QString writePointShapefile(const CsvTable& table, int rowIdx,
                                const QString& siteId, double lon, double lat,
                                int latC, int lonC, QTextStream& log);

    static QString sanitize(const QString& s);
    static QString fileNameFromUrl(const QString& url);
};
