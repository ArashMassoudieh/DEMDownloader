#pragma once
#include "Types.h"
#include "ProductType.h"
#include "TnmClient.h"
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
};

// Walks each site in a CsvTable, resolves the best resolution for each
// requested ProductType (and optionally downloads), and writes an augmented
// output CSV. Holds no global state; one instance per run.
class SiteProcessor {
public:
    SiteProcessor(TnmClient& client,
                  QList<std::shared_ptr<ProductType>> products,
                  ProcessOptions opts);

    // Returns false on a fatal setup error (missing columns, unwritable out).
    bool run(const CsvTable& table, const QString& outPath,
             QTextStream& log, QString* err);

private:
    TnmClient& m_client;
    QList<std::shared_ptr<ProductType>> m_products;
    ProcessOptions m_opts;

    // Resolve one product type for one point (walk tiers, first hit wins).
    ProductOutcome resolveProduct(const ProductType& product,
                                  double lat, double lon,
                                  const QString& siteId,
                                  QTextStream& log);

    // Download the winning tiles for one product into <dir>/<siteId>/<key>/.
    void downloadOutcome(const ProductType& product, ProductOutcome& outcome,
                         const QString& siteId, QTextStream& log);

    static QString sanitize(const QString& s);
    static QString fileNameFromUrl(const QString& url);
};
