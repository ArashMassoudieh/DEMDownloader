// demcheck — query USGS TNMAccess API for the highest-resolution 3DEP DEM
// available at each lat/lon in an input CSV.
//
// Build:   see CMakeLists.txt  (Qt5 or Qt6, Core + Network)
// Usage:   demcheck input.csv [output.csv] [--lat-col NAME] [--lon-col NAME]
//                             [--buffer METERS] [--id-col NAME]
//                             [--download DIR] [--max-tiles N]
//
// Input CSV must have a header row. By default the program looks for columns
// named (case-insensitive) "lat"/"latitude" and "lon"/"longitude"/"lng".
// Override with --lat-col / --lon-col. Coordinates are decimal degrees (WGS84).
//
// Output CSV = input columns + appended:
//   best_resolution, best_dataset, tile_count, best_dem_date,
//   best_download_url, query_status
//   (+ tiles_downloaded, download_dir  when --download is used)
//
// With --download DIR, the matching DEM tiles for each site are saved into
// DIR/<Project No.>/  (one subfolder per project, named from --id-col).
//
// Notes:
//  - The API is queried synchronously, one point at a time, finest->coarsest,
//    stopping at the first resolution tier that returns >=1 tile.
//  - A small bounding box (default 30 m half-width) is built around each point
//    because TNMAccess matches products by extent, not by single coordinate.
//  - 1 m DEM tiles are large (often 100-400 MB each). A wide --buffer over a
//    1 m coverage area can match many tiles; --max-tiles caps how many are
//    pulled per site (default 8). Existing files are skipped on re-run.

#include <QtCore/QCoreApplication>
#include <QtCore/QCommandLineParser>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QtMath>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

#include <vector>

// ---- 3DEP resolution tiers, finest to coarsest --------------------------
// {human label, TNMAccess "datasets" string}.
// If USGS renames a dataset, edit the second field here.
struct Tier { QString label; QString dataset; };
static const std::vector<Tier> RESOLUTIONS = {
    {"1 meter",          "Digital Elevation Model (DEM) 1 meter"},
    {"1/9 arc-second",   "National Elevation Dataset (NED) 1/9 arc-second"},
    {"1/3 arc-second",   "National Elevation Dataset (NED) 1/3 arc-second"},
    {"1 arc-second",     "National Elevation Dataset (NED) 1 arc-second"}
};

static const char* TNM_BASE = "https://tnmaccess.nationalmap.gov/api/v1/products";

// ---- CSV parsing (handles quoted fields with commas) ---------------------
static QStringList parseCsvLine(const QString& line) {
    QStringList out;
    QString cur;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        QChar c = line.at(i);
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line.at(i + 1) == '"') { cur += '"'; ++i; }
                else inQuotes = false;
            } else cur += c;
        } else {
            if (c == '"') inQuotes = true;
            else if (c == ',') { out << cur; cur.clear(); }
            else cur += c;
        }
    }
    out << cur;
    return out;
}

static QString csvEscape(const QString& f) {
    if (f.contains(',') || f.contains('"') || f.contains('\n')) {
        QString e = f; e.replace("\"", "\"\"");
        return "\"" + e + "\"";
    }
    return f;
}

// ---- one synchronous GET, returns parsed JSON object ---------------------
struct Tile {
    QString url;
    QString name;   // suggested filename (from title or URL)
    qint64  bytes = -1;
};
struct QueryResult {
    bool ok = false;
    int tileCount = 0;
    QString date;
    QString url;            // first tile URL (for the CSV summary column)
    QList<Tile> tiles;      // all matching tiles (for downloading)
    QString error;
};

static QueryResult queryTier(QNetworkAccessManager& nam,
                             double lat, double lon, double bufferM,
                             const QString& dataset) {
    QueryResult r;

    // Build a small bbox around the point. ~111320 m per degree lat;
    // lon scaled by cos(lat). bbox order is xmin,ymin,xmax,ymax (lon,lat).
    double dLat = bufferM / 111320.0;
    double cosL = qCos(qDegreesToRadians(lat));
    if (qFabs(cosL) < 1e-6) cosL = 1e-6;
    double dLon = bufferM / (111320.0 * cosL);
    QString bbox = QString("%1,%2,%3,%4")
        .arg(lon - dLon, 0, 'f', 8).arg(lat - dLat, 0, 'f', 8)
        .arg(lon + dLon, 0, 'f', 8).arg(lat + dLat, 0, 'f', 8);

    QUrl url(TNM_BASE);
    QUrlQuery q;
    q.addQueryItem("bbox", bbox);
    q.addQueryItem("datasets", dataset);
    q.addQueryItem("prodFormats", "GeoTIFF");
    q.addQueryItem("outputFormat", "JSON");
    q.addQueryItem("max", "50");   // TNM caps a single request near 50 tiles
    q.addQueryItem("offset", "0");
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "demcheck/1.0 (EnviroInformatics)");

    QNetworkReply* reply = nam.get(req);

    // Block until finished, with a timeout.
    QEventLoop loop;
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(30000); // 30 s
    loop.exec();

    if (!timer.isActive()) { // timed out
        r.error = "timeout";
        reply->abort(); reply->deleteLater();
        return r;
    }
    timer.stop();

    if (reply->error() != QNetworkReply::NoError) {
        r.error = reply->errorString();
        reply->deleteLater();
        return r;
    }

    QByteArray body = reply->readAll();
    reply->deleteLater();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = "bad JSON: " + pe.errorString();
        return r;
    }
    QJsonObject obj = doc.object();
    int total = obj.value("total").toInt(0);
    r.tileCount = total;
    r.ok = true;

    QJsonArray items = obj.value("items").toArray();
    for (const QJsonValue& v : items) {
        QJsonObject it = v.toObject();
        Tile tile;
        QJsonObject urls = it.value("urls").toObject();
        tile.url = urls.value("TIFF").toString();
        if (tile.url.isEmpty()) tile.url = it.value("downloadURL").toString();
        if (tile.url.isEmpty()) continue;
        tile.name = it.value("title").toString();
        // sizeInBytes is sometimes a number, sometimes a string
        QJsonValue sz = it.value("sizeInBytes");
        if (sz.isDouble()) tile.bytes = static_cast<qint64>(sz.toDouble());
        else if (sz.isString()) tile.bytes = sz.toString().toLongLong();
        r.tiles.append(tile);
    }
    if (!items.isEmpty()) {
        QJsonObject first = items.first().toObject();
        r.date = first.value("publicationDate").toString();
        if (r.date.isEmpty()) r.date = first.value("dateCreated").toString();
    }
    if (!r.tiles.isEmpty()) r.url = r.tiles.first().url;
    return r;
}

// ---- filename + directory helpers ---------------------------------------
// Make a string safe to use as a folder name (project IDs are clean, but
// guard against slashes etc. just in case).
static QString sanitize(const QString& s) {
    QString r = s.trimmed();
    r.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
    if (r.isEmpty()) r = "unnamed";
    return r;
}

static QString fileNameFromUrl(const QString& url) {
    QString path = QUrl(url).path();
    QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QString("download.tif") : name;
}

// ---- one synchronous download to disk -----------------------------------
// Returns true on success. Writes to <dest>.part then renames, so a partial
// file is never left looking complete. Skips if dest already exists with the
// expected size (or any size, if expected is unknown).
static bool downloadFile(QNetworkAccessManager& nam, const QString& url,
                         const QString& destPath, qint64 expectedBytes,
                         QTextStream& log) {
    QFileInfo fi(destPath);
    if (fi.exists() && fi.size() > 0 &&
        (expectedBytes < 0 || fi.size() == expectedBytes)) {
        log << "      exists, skipping: " << fi.fileName() << "\n"; log.flush();
        return true;
    }

    QString partPath = destPath + ".part";
    QFile f(partPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        log << "      cannot write: " << partPath << "\n"; log.flush();
        return false;
    }

    QString current = url;
    bool success = false;
    for (int redirect = 0; redirect < 5; ++redirect) {
        QNetworkRequest req((QUrl(current)));
        req.setHeader(QNetworkRequest::UserAgentHeader, "demcheck/1.1 (EnviroInformatics)");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam.get(req);

        QEventLoop loop;
        QTimer timer; timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        // Stream bytes to disk as they arrive rather than buffering in RAM
        QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
            f.write(reply->readAll());
            timer.start(120000); // reset inactivity timer on each chunk
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(120000); // 2 min inactivity timeout
        loop.exec();

        if (!timer.isActive()) {
            log << "      timeout\n"; log.flush();
            reply->abort(); reply->deleteLater();
            break;
        }
        timer.stop();
        f.write(reply->readAll()); // flush any remainder

        if (reply->error() != QNetworkReply::NoError) {
            log << "      error: " << reply->errorString() << "\n"; log.flush();
            reply->deleteLater();
            break;
        }
        reply->deleteLater();
        success = true;
        break;
    }

    f.close();
    if (success && f.size() > 0) {
        if (QFile::exists(destPath)) QFile::remove(destPath);
        if (QFile::rename(partPath, destPath)) return true;
        log << "      cannot finalize: " << destPath << "\n"; log.flush();
        return false;
    }
    f.remove(); // drop the .part on failure
    return false;
}

static int findCol(const QStringList& header, const QStringList& candidates,
                   const QString& override) {
    if (!override.isEmpty()) {
        for (int i = 0; i < header.size(); ++i)
            if (header.at(i).trimmed().compare(override, Qt::CaseInsensitive) == 0) return i;
        return -1;
    }
    for (int i = 0; i < header.size(); ++i)
        for (const QString& c : candidates)
            if (header.at(i).trimmed().compare(c, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("demcheck");

    QCommandLineParser p;
    p.setApplicationDescription("Find highest-resolution USGS 3DEP DEM at each coordinate in a CSV.");
    p.addHelpOption();
    p.addPositionalArgument("input", "Input CSV (with header row).");
    p.addPositionalArgument("output", "Output CSV (optional; default <input>_dem.csv).");
    QCommandLineOption latOpt("lat-col", "Latitude column name.", "name");
    QCommandLineOption lonOpt("lon-col", "Longitude column name.", "name");
    QCommandLineOption idOpt("id-col", "Identifier column to echo in console log.", "name");
    QCommandLineOption bufOpt("buffer", "Half-width of query box in meters (default 30).", "meters", "30");
    QCommandLineOption dlOpt("download", "Download DEM tiles into this directory (subfolder per project).", "dir");
    QCommandLineOption maxTilesOpt("max-tiles", "Max tiles to download per site (default 8; 0 = no limit).", "n", "8");
    p.addOption(latOpt); p.addOption(lonOpt); p.addOption(idOpt); p.addOption(bufOpt);
    p.addOption(dlOpt); p.addOption(maxTilesOpt);
    p.process(app);

    const QStringList args = p.positionalArguments();
    if (args.isEmpty()) { p.showHelp(1); }

    const QString inPath = args.at(0);
    const QString outPath = args.size() > 1 ? args.at(1)
        : (inPath.endsWith(".csv", Qt::CaseInsensitive)
            ? inPath.left(inPath.size()-4) + "_dem.csv" : inPath + "_dem.csv");
    const double bufferM = p.value(bufOpt).toDouble();
    const QString dlDir = p.value(dlOpt);            // empty => no download
    const int maxTiles = p.value(maxTilesOpt).toInt();
    const bool doDownload = !dlDir.isEmpty();
    if (doDownload) {
        QDir().mkpath(dlDir);
        if (!QFileInfo(dlDir).isDir()) {
            QTextStream(stderr) << "Cannot create download dir: " << dlDir << "\n";
            return 2;
        }
    }

    QFile in(inPath);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream(stderr) << "Cannot open input: " << inPath << "\n"; return 2;
    }
    QTextStream ts(&in);
    QStringList lines;
    while (!ts.atEnd()) lines << ts.readLine();
    in.close();
    if (lines.isEmpty()) { QTextStream(stderr) << "Empty file.\n"; return 2; }

    QStringList header = parseCsvLine(lines.first());
    int latC = findCol(header, {"lat","latitude","centroid lat","centroid_lat","site lat"}, p.value(latOpt));
    int lonC = findCol(header, {"lon","lng","longitude","centroid lon","centroid_lon","site lon"}, p.value(lonOpt));
    int idC  = findCol(header, {}, p.value(idOpt));
    if (latC < 0 || lonC < 0) {
        QTextStream(stderr) << "Could not find lat/lon columns. Use --lat-col / --lon-col.\n"
                            << "Header was: " << header.join(" | ") << "\n";
        return 2;
    }

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream(stderr) << "Cannot open output: " << outPath << "\n"; return 2;
    }
    QTextStream os(&out);

    QStringList newCols = {"best_resolution","best_dataset","tile_count",
                           "best_dem_date","best_download_url","query_status"};
    if (doDownload) newCols << "tiles_downloaded" << "download_dir";
    QStringList outHeader = header;
    outHeader << newCols;
    QStringList esc; for (const QString& h : outHeader) esc << csvEscape(h);
    os << esc.join(",") << "\n";

    QNetworkAccessManager nam;
    QTextStream log(stdout);

    for (int li = 1; li < lines.size(); ++li) {
        if (lines.at(li).trimmed().isEmpty()) continue;
        QStringList fields = parseCsvLine(lines.at(li));
        while (fields.size() < header.size()) fields << "";

        bool okLat=false, okLon=false;
        double lat = fields.value(latC).trimmed().toDouble(&okLat);
        double lon = fields.value(lonC).trimmed().toDouble(&okLon);
        QString idStr = (idC >= 0) ? fields.value(idC) : QString("row %1").arg(li);

        QString bestRes="none", bestDs="", date="", durl="", status="no coverage";
        int tiles = 0;
        bool anyError = false, anyOk = false;
        QList<Tile> winningTiles;

        if (!okLat || !okLon) {
            status = "missing/invalid coordinate";
            log << idStr << ": " << status << "\n"; log.flush();
        } else {
            for (const Tier& t : RESOLUTIONS) {
                QueryResult qr = queryTier(nam, lat, lon, bufferM, t.dataset);
                if (!qr.ok) { anyError = true;
                    log << idStr << " [" << t.label << "]: " << qr.error << "\n"; log.flush();
                    continue; // try next tier; transient error shouldn't abort
                }
                anyOk = true;
                if (qr.tileCount > 0) {
                    bestRes = t.label; bestDs = t.dataset; tiles = qr.tileCount;
                    date = qr.date; durl = qr.url; status = "ok";
                    winningTiles = qr.tiles;
                    break;
                }
            }
            // If we never got a hit: distinguish "confirmed no coverage"
            // (all tiers answered cleanly) from "results unreliable" (some
            // tier errored, so a finer DEM might exist but went unchecked).
            if (status != "ok") {
                if (anyError && bestRes == "none")
                    status = anyOk ? "incomplete: some tiers errored"
                                   : "API error: no tiers reachable";
                else
                    status = "no coverage";
            }
            log << idStr << ": " << bestRes
                << (tiles ? QString(" (%1 tile(s))").arg(tiles) : QString())
                << " [" << status << "]\n"; log.flush();
        }

        // ---- download winning tiles into <dlDir>/<project>/ --------------
        int downloaded = 0;
        QString siteDir;
        if (doDownload && status == "ok" && !winningTiles.isEmpty()) {
            // Folder name: prefer the id column; fall back to row index.
            QString folder = (idC >= 0 && !fields.value(idC).trimmed().isEmpty())
                ? sanitize(fields.value(idC)) : QString("row_%1").arg(li);
            siteDir = QDir(dlDir).filePath(folder);
            QDir().mkpath(siteDir);

            int limit = (maxTiles > 0) ? maxTiles : winningTiles.size();
            int toGet = qMin(limit, winningTiles.size());
            if (winningTiles.size() > toGet)
                log << "   " << winningTiles.size() << " tiles available, downloading first "
                    << toGet << " (raise --max-tiles to get more)\n";
            for (int k = 0; k < toGet; ++k) {
                const Tile& tl = winningTiles.at(k);
                QString fname = !tl.name.isEmpty() ? sanitize(tl.name) + ".tif"
                                                   : fileNameFromUrl(tl.url);
                if (!fname.endsWith(".tif", Qt::CaseInsensitive) &&
                    !fname.endsWith(".tiff", Qt::CaseInsensitive))
                    fname = fileNameFromUrl(tl.url);
                QString dest = QDir(siteDir).filePath(fname);
                log << "   downloading [" << (k+1) << "/" << toGet << "] " << fname << "\n"; log.flush();
                if (downloadFile(nam, tl.url, dest, tl.bytes, log)) downloaded++;
            }
            log << "   -> " << downloaded << " file(s) in " << siteDir << "\n"; log.flush();
        }

        QStringList row = fields;
        row << bestRes << bestDs << QString::number(tiles) << date << durl << status;
        if (doDownload) row << QString::number(downloaded) << siteDir;
        QStringList rowEsc; for (const QString& f : row) rowEsc << csvEscape(f);
        os << rowEsc.join(",") << "\n"; os.flush();
    }

    out.close();
    log << "\nDone. Wrote: " << outPath << "\n";
    return 0;
}
