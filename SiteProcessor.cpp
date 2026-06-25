#include "SiteProcessor.h"
#include "CsvTable.h"
#include "ShapefileWriter.h"
#include "TigerClient.h"
#include "MrlcClient.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
#include <QtCore/QTextStream>
#include <QtCore/QRegularExpression>

SiteProcessor::SiteProcessor(TnmClient& client,
                             TigerClient& tiger,
                             MrlcClient& mrlc,
                             QList<std::shared_ptr<ProductType>> products,
                             ProcessOptions opts)
    : m_client(client), m_tiger(tiger), m_mrlc(mrlc),
    m_products(std::move(products)), m_opts(std::move(opts)) {}

QString SiteProcessor::sanitize(const QString& s) {
    QString r = s.trimmed();
    r.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
    if (r.isEmpty()) r = "unnamed";
    return r;
}

QString SiteProcessor::fileNameFromUrl(const QString& url) {
    const QString path = QUrl(url).path();
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QString("download.dat") : name;
}

// ---------------------------------------------------------------------------
// TNM product resolution
// ---------------------------------------------------------------------------

ProductOutcome SiteProcessor::resolveProduct(const ProductType& product,
                                             double lat, double lon,
                                             const QString& siteId,
                                             QTextStream& log) {
    ProductOutcome out;
    bool anyError = false, anyOk = false;

    for (const Tier& t : product.tiers()) {
        QueryResult qr = m_client.query(lat, lon, m_opts.bufferMeters,
                                        t.dataset, product.prodFormats());
        if (!qr.ok) {
            anyError = true;
            log << "  " << siteId << " [" << product.key() << "/" << t.label
                << "]: " << qr.error << "\n"; log.flush();
            continue;
        }
        anyOk = true;
        if (qr.tileCount > 0) {
            out.bestResolution = t.label;
            out.bestDataset    = t.dataset;
            out.tileCount      = qr.tileCount;
            out.date           = qr.date;
            out.firstUrl       = qr.firstUrl;
            out.status         = "ok";
            out.tiles          = qr.tiles;
            break;
        }
    }

    if (out.status != "ok") {
        if (anyError && out.bestResolution == "none")
            out.status = anyOk ? "incomplete: some tiers errored"
                               : "API error: no tiers reachable";
        else
            out.status = "no coverage";
    }

    log << "  " << siteId << " [" << product.key() << "]: " << out.bestResolution
        << (out.tileCount ? QString(" (%1 product(s))").arg(out.tileCount) : QString())
        << " [" << out.status << "]\n"; log.flush();
    return out;
}

void SiteProcessor::downloadOutcome(const ProductType& product,
                                    ProductOutcome& outcome,
                                    const QString& siteId, QTextStream& log) {
    if (m_opts.downloadDir.isEmpty() || outcome.status != "ok" || outcome.tiles.isEmpty())
        return;

    const QString siteDir = QDir(m_opts.downloadDir).filePath(sanitize(siteId));
    const QString prodDir = QDir(siteDir).filePath(product.key());
    QDir().mkpath(prodDir);
    outcome.downloadDir = prodDir;

    int cap   = (m_opts.maxTiles >= 0) ? m_opts.maxTiles : product.defaultMaxTiles();
    int limit = (cap > 0) ? cap : outcome.tiles.size();
    int toGet = qMin(limit, outcome.tiles.size());

    if (outcome.tiles.size() > toGet)
        log << "  " << outcome.tiles.size() << " files available, downloading first "
            << toGet << " (raise --max-tiles for more)\n";

    for (int k = 0; k < toGet; ++k) {
        const Tile& tl = outcome.tiles.at(k);
        QString fname = fileNameFromUrl(tl.url);
        if (fname == "download.dat" && !tl.name.isEmpty())
            fname = sanitize(tl.name);
        const QString dest = QDir(prodDir).filePath(fname);
        log << "  downloading [" << (k + 1) << "/" << toGet << "] " << fname << "\n"; log.flush();
        if (m_client.download(tl.url, dest, tl.bytes, &log)) outcome.downloaded++;
    }
    log << "  -> " << outcome.downloaded << " file(s) in " << prodDir << "\n"; log.flush();
}

// ---------------------------------------------------------------------------
// Roads (TIGER/Line)
// ---------------------------------------------------------------------------

ProductOutcome SiteProcessor::resolveAndDownloadRoads(const ProductType& product,
                                                      double lat, double lon,
                                                      const QString& siteId,
                                                      QTextStream& log) {
    ProductOutcome out;
    out.bestDataset = "Census TIGER/Line All Roads";

    QString countyName, gerr;
    const QString fips = m_tiger.countyFipsForPoint(lat, lon, &countyName, &gerr);
    if (fips.isEmpty()) {
        out.status = "geocode failed: " + gerr;
        log << "  " << siteId << " [" << product.key() << "]: " << out.status << "\n"; log.flush();
        return out;
    }

    out.bestResolution = countyName.isEmpty() ? fips : countyName;
    out.firstUrl       = m_tiger.roadsUrlForFips(fips);
    out.tileCount      = 1;
    out.status         = "ok";
    log << "  " << siteId << " [" << product.key() << "]: county " << fips
        << (countyName.isEmpty() ? QString() : " (" + countyName + ")")
        << "\n"; log.flush();

    if (m_opts.downloadDir.isEmpty()) return out;

    const QString siteDir = QDir(m_opts.downloadDir).filePath(sanitize(siteId));
    const QString prodDir = QDir(siteDir).filePath(product.key());
    QDir().mkpath(prodDir);
    out.downloadDir = prodDir;

    const QString fname = fileNameFromUrl(out.firstUrl);
    const QString dest  = QDir(prodDir).filePath(fname);
    log << "  downloading " << fname << "\n"; log.flush();
    if (m_tiger.download(out.firstUrl, dest, -1, &log)) {
        out.downloaded = 1;
        log << "  -> 1 file in " << prodDir << "\n"; log.flush();
    } else {
        out.status = "download failed (county file missing?)";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Land cover (NLCD / MRLC WCS)
// ---------------------------------------------------------------------------

ProductOutcome SiteProcessor::resolveAndDownloadLandCover(const ProductType& product,
                                                          double lat, double lon,
                                                          const QString& siteId,
                                                          QTextStream& log) {
    ProductOutcome out;
    out.bestDataset    = QString("NLCD Annual Land Cover %1 (MRLC WCS)").arg(m_mrlc.year());
    out.bestResolution = "30 m";
    out.tileCount      = 1;
    out.firstUrl       = m_mrlc.coverageUrlForBbox(lat, lon, m_opts.bufferMeters);
    out.status         = "ok";
    out.date           = QString::number(m_mrlc.year());

    log << "  " << siteId << " [" << product.key() << "]: NLCD "
        << m_mrlc.year() << " (30 m), bbox subset\n"; log.flush();

    if (m_opts.downloadDir.isEmpty()) return out;

    const QString siteDir = QDir(m_opts.downloadDir).filePath(sanitize(siteId));
    const QString prodDir = QDir(siteDir).filePath(product.key());
    QDir().mkpath(prodDir);
    out.downloadDir = prodDir;

    const QString fname = QString("nlcd_%1_%2.tif").arg(m_mrlc.year()).arg(sanitize(siteId));
    const QString dest  = QDir(prodDir).filePath(fname);
    log << "  downloading " << fname << "\n"; log.flush();
    if (m_mrlc.download(out.firstUrl, dest, &log)) {
        out.downloaded = 1;
        log << "  -> 1 file in " << prodDir << "\n"; log.flush();
    } else {
        out.status = "download failed (check coverage id / axis labels / year)";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Atlas 14
// ---------------------------------------------------------------------------
//
// Folder layout (mirrors every other layer):
//   <download_dir>/<siteId>/atlas14/atlas14_pf.csv
//
// atlas14_pf.csv format:
//   duration,2yr,5yr,10yr,25yr,50yr,100yr
//   5min,0.23,0.31,...
//   ...
//   6hr,1.10,1.45,...
//   24hr,1.55,2.05,...
//
// The file is written only when --download is set.  The CSV columns
// atlas14_6hr_100yr_in and atlas14_24hr_100yr_in are always written.
// atlas14_downloaded and atlas14_dir are added (like other layers) when
// --download is set.

// Duration order matching the HDSC response for a clean output file.
static const QStringList ATLAS14_DUR_ORDER = {
    "5min","10min","15min","30min","60min",
    "2hr","3hr","6hr","12hr","24hr",
    "2day","3day","4day","7day","10day","20day","30day","45day","60day"
};
static const QStringList ATLAS14_FREQ_ORDER = {
    "2yr","5yr","10yr","25yr","50yr","100yr"
};

Atlas14Result SiteProcessor::resolveAndDownloadAtlas14(double lat, double lon,
                                                       const QString& siteId,
                                                       QTextStream& log) {
    log << "  " << siteId << " [atlas14]: querying NOAA HDSC (" << lat << ", " << lon << ")... ";
    log.flush();

    Atlas14Result r = m_atlas14.query(lat, lon);

    if (!r.ok) {
        log << "error: " << r.error << "\n"; log.flush();
        return r;
    }

    log << "ok  6hr/100yr=" << QString::number(r.depth6hr100yr(),  'f', 2) << "\""
        << "  24hr/100yr=" << QString::number(r.depth24hr100yr(), 'f', 2) << "\"\n";
    log.flush();

    // Write per-site file only when --download is active.
    if (m_opts.downloadDir.isEmpty()) return r;

    const QString siteDir = QDir(m_opts.downloadDir).filePath(sanitize(siteId));
    const QString prodDir = QDir(siteDir).filePath("atlas14");
    QDir().mkpath(prodDir);
    r.downloadDir = prodDir;

    const QString dest = QDir(prodDir).filePath("atlas14_pf.csv");

    QFile f(dest);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        log << "  cannot write: " << dest << "\n"; log.flush();
        // Not fatal — depths still go in the main CSV.
        return r;
    }

    QTextStream ts(&f);

    // Header row.
    ts << "duration";
    for (const QString& rp : ATLAS14_FREQ_ORDER) ts << "," << rp;
    ts << "\n";

    // One row per duration, in canonical order.  If a duration came back from
    // HDSC but isn't in our order list, append it at the end so nothing is lost.
    QStringList durs = ATLAS14_DUR_ORDER;
    for (const QString& d : r.table.keys())
        if (!durs.contains(d)) durs << d;

    for (const QString& dur : durs) {
        if (!r.table.contains(dur)) continue;
        ts << dur;
        const QMap<QString, double>& freqMap = r.table[dur];
        for (const QString& rp : ATLAS14_FREQ_ORDER) {
            if (freqMap.contains(rp))
                ts << "," << QString::number(freqMap[rp], 'f', 2);
            else
                ts << ",";
        }
        ts << "\n";
    }
    f.close();

    r.downloaded = 1;
    log << "  -> 1 file in " << prodDir << "\n"; log.flush();
    return r;
}

// ---------------------------------------------------------------------------
// Point shapefiles
// ---------------------------------------------------------------------------

QString SiteProcessor::writePointShapefile(const CsvTable& table, int rowIdx,
                                           const QString& siteId,
                                           double lon, double lat,
                                           int latC, int lonC, QTextStream& log) {
    QString base = !m_opts.downloadDir.isEmpty() ? m_opts.downloadDir : m_opts.pointsDir;
    if (base.isEmpty()) base = ".";

    const QString siteDir = QDir(base).filePath(sanitize(siteId));
    const QString ptDir   = QDir(siteDir).filePath("point");
    QDir().mkpath(ptDir);

    QList<ShapefileWriter::Attr> attrs;
    QStringList usedNames;
    const QStringList& hdr = table.header();
    for (int c = 0; c < hdr.size(); ++c) {
        if (c == latC || c == lonC) continue;
        QString name = sanitize(hdr.at(c)).left(10).trimmed();
        name.replace(' ', '_');
        if (name.isEmpty()) name = QString("F%1").arg(c);
        QString unique = name; int n = 1;
        while (usedNames.contains(unique, Qt::CaseInsensitive))
            unique = name.left(8) + QString("_%1").arg(n++);
        usedNames << unique;
        attrs << ShapefileWriter::Attr{ unique, table.field(rowIdx, c) };
    }

    const QString basePath = QDir(ptDir).filePath(sanitize(siteId));
    QString err;
    if (!ShapefileWriter::writePoint(basePath, lon, lat, attrs, &err)) {
        log << "  " << siteId << " [point]: " << err << "\n"; log.flush();
        return QString();
    }
    log << "  " << siteId << " [point]: wrote " << basePath << ".shp\n"; log.flush();
    return ptDir;
}

// ---------------------------------------------------------------------------
// Main run loop
// ---------------------------------------------------------------------------

bool SiteProcessor::run(const CsvTable& table, const QString& outPath,
                        QTextStream& log, QString* err) {
    const int latC = table.findColumn(
        {"lat","latitude","centroid lat","centroid_lat","site lat"}, m_opts.latColOverride);
    const int lonC = table.findColumn(
        {"lon","lng","longitude","centroid lon","centroid_lon","site lon"}, m_opts.lonColOverride);
    const int idC  = table.findColumn({}, m_opts.idColOverride);

    if (latC < 0 || lonC < 0) {
        if (err) *err = "could not find lat/lon columns; use --lat-col / --lon-col.\n"
                   "Header was: " + table.header().join(" | ");
        return false;
    }

    const bool doDownload = !m_opts.downloadDir.isEmpty();
    if (doDownload) {
        QDir().mkpath(m_opts.downloadDir);
        if (!QFileInfo(m_opts.downloadDir).isDir()) {
            if (err) *err = "cannot create download dir: " + m_opts.downloadDir;
            return false;
        }
    }

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (err) *err = "cannot open output: " + outPath;
        return false;
    }
    QTextStream os(&out);

    // Build output header.
    QStringList outHeader = table.header();

    for (const auto& p : m_products) {
        const QString k = p->key();
        outHeader << k + "_best_resolution" << k + "_best_dataset"
                  << k + "_count" << k + "_date" << k + "_url" << k + "_status";
        if (doDownload) outHeader << k + "_downloaded" << k + "_dir";
    }

    // Atlas 14 columns — always appended when atlas14 is requested.
    if (m_opts.doAtlas14) {
        outHeader << "atlas14_6hr_100yr_in"
                  << "atlas14_24hr_100yr_in"
                  << "atlas14_status";
        if (doDownload) outHeader << "atlas14_downloaded" << "atlas14_dir";
    }

    if (m_opts.makePoints) outHeader << "point_dir";

    QStringList he; for (const QString& h : outHeader) he << CsvTable::escape(h);
    os << he.join(",") << "\n"; os.flush();

    for (int i = 0; i < table.rowCount(); ++i) {
        bool okLat = false, okLon = false;
        const double lat = table.field(i, latC).trimmed().toDouble(&okLat);
        const double lon = table.field(i, lonC).trimmed().toDouble(&okLon);
        const QString siteId = (idC >= 0 && !table.field(i, idC).trimmed().isEmpty())
                                   ? table.field(i, idC) : QString("row_%1").arg(i + 1);

        log << siteId << ":\n"; log.flush();

        QStringList row = table.row(i);

        // TNM / TIGER / MRLC products.
        for (const auto& p : m_products) {
            ProductOutcome oc;
            if (!okLat || !okLon) {
                oc.status = "missing/invalid coordinate";
                log << "  " << siteId << " [" << p->key() << "]: " << oc.status << "\n"; log.flush();
            } else if (p->fetchVia() == FetchVia::County) {
                oc = resolveAndDownloadRoads(*p, lat, lon, siteId, log);
            } else if (p->fetchVia() == FetchVia::BboxRaster) {
                oc = resolveAndDownloadLandCover(*p, lat, lon, siteId, log);
            } else {
                oc = resolveProduct(*p, lat, lon, siteId, log);
                downloadOutcome(*p, oc, siteId, log);
            }
            row << oc.bestResolution << oc.bestDataset << QString::number(oc.tileCount)
                << oc.date << oc.firstUrl << oc.status;
            if (doDownload) row << QString::number(oc.downloaded) << oc.downloadDir;
        }

        // Atlas 14.
        if (m_opts.doAtlas14) {
            if (!okLat || !okLon) {
                row << "" << "" << "missing/invalid coordinate";
                if (doDownload) row << "0" << "";
            } else {
                Atlas14Result pf = resolveAndDownloadAtlas14(lat, lon, siteId, log);
                if (pf.ok) {
                    row << QString::number(pf.depth6hr100yr(),  'f', 2)
                    << QString::number(pf.depth24hr100yr(), 'f', 2)
                    << "ok";
                } else {
                    row << "" << "" << ("error: " + pf.error);
                }
                if (doDownload) row << QString::number(pf.downloaded) << pf.downloadDir;
            }
        }

        if (m_opts.makePoints) {
            QString ptDir;
            if (okLat && okLon)
                ptDir = writePointShapefile(table, i, siteId, lon, lat, latC, lonC, log);
            else
                log << "  " << siteId << " [point]: skipped (invalid coordinate)\n";
            row << ptDir;
        }

        QStringList re; for (const QString& f : row) re << CsvTable::escape(f);
        os << re.join(",") << "\n"; os.flush();
    }

    out.close();
    log << "\nDone. Wrote: " << outPath << "\n";
    return true;
}
