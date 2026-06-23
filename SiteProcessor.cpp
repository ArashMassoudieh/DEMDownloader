#include "SiteProcessor.h"
#include "CsvTable.h"
#include "ShapefileWriter.h"
#include "TigerClient.h"
#include "MrlcClient.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
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
            log << "   " << siteId << " [" << product.key() << "/" << t.label
                << "]: " << qr.error << "\n"; log.flush();
            continue; // transient error shouldn't abort the whole product
        }
        anyOk = true;
        if (qr.tileCount > 0) {
            out.bestResolution = t.label;
            out.bestDataset = t.dataset;
            out.tileCount = qr.tileCount;
            out.date = qr.date;
            out.firstUrl = qr.firstUrl;
            out.status = "ok";
            out.tiles = qr.tiles;
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
    log << "   " << siteId << " [" << product.key() << "]: " << out.bestResolution
        << (out.tileCount ? QString(" (%1 product(s))").arg(out.tileCount) : QString())
        << " [" << out.status << "]\n"; log.flush();
    return out;
}

void SiteProcessor::downloadOutcome(const ProductType& product,
                                    ProductOutcome& outcome,
                                    const QString& siteId, QTextStream& log) {
    if (m_opts.downloadDir.isEmpty() || outcome.status != "ok" || outcome.tiles.isEmpty())
        return;

    // <dir>/<siteId>/<productKey>/
    const QString siteDir = QDir(m_opts.downloadDir).filePath(sanitize(siteId));
    const QString prodDir = QDir(siteDir).filePath(product.key());
    QDir().mkpath(prodDir);
    outcome.downloadDir = prodDir;

    int cap = (m_opts.maxTiles >= 0) ? m_opts.maxTiles : product.defaultMaxTiles();
    int limit = (cap > 0) ? cap : outcome.tiles.size();
    int toGet = qMin(limit, outcome.tiles.size());
    if (outcome.tiles.size() > toGet)
        log << "      " << outcome.tiles.size() << " files available, downloading first "
            << toGet << " (raise --max-tiles for more)\n";

    for (int k = 0; k < toGet; ++k) {
        const Tile& tl = outcome.tiles.at(k);
        QString fname = fileNameFromUrl(tl.url);
        // If the URL gave no useful name, fall back to the item title.
        if (fname == "download.dat" && !tl.name.isEmpty())
            fname = sanitize(tl.name);
        const QString dest = QDir(prodDir).filePath(fname);
        log << "      downloading [" << (k + 1) << "/" << toGet << "] " << fname << "\n"; log.flush();
        if (m_client.download(tl.url, dest, tl.bytes, &log)) outcome.downloaded++;
    }
    log << "      -> " << outcome.downloaded << " file(s) in " << prodDir << "\n"; log.flush();
}

QString SiteProcessor::writePointShapefile(const CsvTable& table, int rowIdx,
                                           const QString& siteId,
                                           double lon, double lat,
                                           int latC, int lonC, QTextStream& log) {
    // Base directory: prefer the download dir (keeps everything together under
    // the site), else the dedicated points dir.
    QString base = !m_opts.downloadDir.isEmpty() ? m_opts.downloadDir : m_opts.pointsDir;
    if (base.isEmpty()) base = ".";
    const QString siteDir = QDir(base).filePath(sanitize(siteId));
    const QString ptDir = QDir(siteDir).filePath("point");
    QDir().mkpath(ptDir);

    // Attributes: carry every CSV column except the raw lat/lon (already the
    // geometry). dBASE field names are capped at 10 chars; de-duplicate.
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
        log << "   " << siteId << " [point]: " << err << "\n"; log.flush();
        return QString();
    }
    log << "   " << siteId << " [point]: wrote " << basePath << ".shp\n"; log.flush();
    return ptDir;
}


ProductOutcome SiteProcessor::resolveAndDownloadRoads(const ProductType& product,
                                                      double lat, double lon,
                                                      const QString& siteId,
                                                      QTextStream& log) {
    ProductOutcome out;
    out.bestDataset = "Census TIGER/Line All Roads";

    // 1) geocode the point to a county FIPS
    QString countyName, gerr;
    const QString fips = m_tiger.countyFipsForPoint(lat, lon, &countyName, &gerr);
    if (fips.isEmpty()) {
        out.status = "geocode failed: " + gerr;
        log << "   " << siteId << " [" << product.key() << "]: " << out.status
            << "\n"; log.flush();
        return out;
    }
    out.bestResolution = countyName.isEmpty() ? fips : countyName;
    out.firstUrl = m_tiger.roadsUrlForFips(fips);
    out.tileCount = 1;
    out.status = "ok";
    log << "   " << siteId << " [" << product.key() << "]: county " << fips
        << (countyName.isEmpty() ? QString() : " (" + countyName + ")")
        << "\n"; log.flush();

    // 2) download the county roads zip (if downloading is enabled)
    if (m_opts.downloadDir.isEmpty())
        return out;

    const QString siteDir = QDir(m_opts.downloadDir).filePath(sanitize(siteId));
    const QString prodDir = QDir(siteDir).filePath(product.key());
    QDir().mkpath(prodDir);
    out.downloadDir = prodDir;

    const QString fname = fileNameFromUrl(out.firstUrl); // tl_YYYY_FIPS_roads.zip
    const QString dest  = QDir(prodDir).filePath(fname);
    log << "      downloading " << fname << "\n"; log.flush();
    if (m_tiger.download(out.firstUrl, dest, -1, &log)) {
        out.downloaded = 1;
        log << "      -> 1 file in " << prodDir << "\n"; log.flush();
    } else {
        // county file may genuinely not exist (rare); report but don't abort
        out.status = "download failed (county file missing?)";
    }
    return out;
}

ProductOutcome SiteProcessor::resolveAndDownloadLandCover(const ProductType& product,
                                                         double lat, double lon,
                                                         const QString& siteId,
                                                         QTextStream& log) {
    ProductOutcome out;
    out.bestDataset = QString("NLCD Annual Land Cover %1 (MRLC WCS)").arg(m_mrlc.year());
    out.bestResolution = "30 m";
    out.tileCount = 1;
    out.firstUrl = m_mrlc.coverageUrlForBbox(lat, lon, m_opts.bufferMeters);
    out.status = "ok";
    out.date = QString::number(m_mrlc.year());
    log << "   " << siteId << " [" << product.key() << "]: NLCD "
        << m_mrlc.year() << " (30 m), bbox subset\n"; log.flush();

    if (m_opts.downloadDir.isEmpty())
        return out;

    const QString siteDir = QDir(m_opts.downloadDir).filePath(sanitize(siteId));
    const QString prodDir = QDir(siteDir).filePath(product.key());
    QDir().mkpath(prodDir);
    out.downloadDir = prodDir;

    const QString fname = QString("nlcd_%1_%2.tif")
                              .arg(m_mrlc.year()).arg(sanitize(siteId));
    const QString dest  = QDir(prodDir).filePath(fname);
    log << "      downloading " << fname << "\n"; log.flush();
    if (m_mrlc.download(out.firstUrl, dest, &log)) {
        out.downloaded = 1;
        log << "      -> 1 file in " << prodDir << "\n"; log.flush();
    } else {
        out.status = "download failed (check coverage id / axis labels / year)";
    }
    return out;
}

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

    // Build header: original columns + per-product columns.
    QStringList outHeader = table.header();
    for (const auto& p : m_products) {
        const QString k = p->key();
        outHeader << k + "_best_resolution" << k + "_best_dataset"
                  << k + "_count" << k + "_date" << k + "_url" << k + "_status";
        if (doDownload) outHeader << k + "_downloaded" << k + "_dir";
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
        for (const auto& p : m_products) {
            ProductOutcome oc;
            if (!okLat || !okLon) {
                oc.status = "missing/invalid coordinate";
                log << "   " << siteId << " [" << p->key() << "]: " << oc.status << "\n"; log.flush();
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

        if (m_opts.makePoints) {
            QString ptDir;
            if (okLat && okLon)
                ptDir = writePointShapefile(table, i, siteId, lon, lat, latC, lonC, log);
            else
                log << "   " << siteId << " [point]: skipped (invalid coordinate)\n";
            row << ptDir;
        }
        QStringList re; for (const QString& f : row) re << CsvTable::escape(f);
        os << re.join(",") << "\n"; os.flush();
    }

    out.close();
    log << "\nDone. Wrote: " << outPath << "\n";
    return true;
}
