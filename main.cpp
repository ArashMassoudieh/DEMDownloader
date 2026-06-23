// demcheck — query the USGS TNMAccess API for the highest-resolution data
// available at each coordinate in a CSV, and optionally download it.
//
// Supports multiple product types:
//   elevation   (3DEP DEM)          — 1 m / 1-9 / 1-3 / 1 arc-second GeoTIFF
//   hydrography (NHDPlus HR / NHD)   — flowlines as zipped Shapefile packages
//   roads       (Census TIGER/Line)  — county "All Roads" Shapefile packages
//
// For each site and each selected product, it walks resolution tiers finest
// to coarsest, stops at the first with coverage, records the result, and (with
// --download) saves the files into <dir>/<site>/<product>/.
//
// Roads are different: the USGS National Transportation Dataset is unusable
// through TNMAccess (its product query errors server-side), so roads come from
// Census TIGER/Line, fetched PER COUNTY. RoadsProduct::isCountyBased() returns
// true and SiteProcessor routes it to TigerClient (geocode -> county FIPS ->
// download tl_<year>_<FIPS>_roads.zip) instead of TnmClient.
//
// Design: main() only parses arguments and wires objects. The work lives in
//   ProductType   — describes a product's tiers and formats (one subclass each)
//   TnmClient     — all TNMAccess HTTP (querying and downloading)
//   TigerClient   — Census geocode + TIGER/Line roads download (county-based)
//   CsvTable      — CSV read / column detection / write
//   SiteProcessor — orchestration across sites and products
//
// Build: qmake demcheck.pro && make   (Qt5 or Qt6, Core + Network)

#include <QtCore/QCoreApplication>
#include <QtCore/QCommandLineParser>
#include <QtCore/QTextStream>
#include <memory>

#include "Types.h"
#include "ProductType.h"
#include "TnmClient.h"
#include "TigerClient.h"
#include "MrlcClient.h"
#include "CsvTable.h"
#include "SiteProcessor.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("demcheck");
    QCoreApplication::setApplicationVersion("2.1");

    QCommandLineParser p;
    p.setApplicationDescription(
        "Find (and optionally download) the highest-resolution USGS 3DEP DEM,\n"
        "NHD hydrography flowlines, and/or Census TIGER/Line roads at each\n"
        "coordinate in a CSV.");
    p.addHelpOption();
    p.addVersionOption();
    p.addPositionalArgument("input", "Input CSV (with header row).");
    p.addPositionalArgument("output", "Output CSV (optional; default <input>_dem.csv).");

    QCommandLineOption latOpt("lat-col", "Latitude column name.", "name");
    QCommandLineOption lonOpt("lon-col", "Longitude column name.", "name");
    QCommandLineOption idOpt("id-col", "Identifier column; also names download subfolders.", "name");
    QCommandLineOption bufOpt("buffer", "Half-width of query box in meters (default 30).", "meters", "30");
    QCommandLineOption dlOpt("download", "Download data into this directory.", "dir");
    QCommandLineOption maxOpt("max-tiles", "Max files per product per site (0 = no limit; default per-product).", "n", "-1");
    QCommandLineOption prodOpt("products", "Comma list: dem,demlr,hydro,roads,landcover,all (default dem).", "list", "dem");
    QCommandLineOption yearOpt("tiger-year", "Census TIGER/Line vintage year for roads (default 2025).", "year", "2025");
    QCommandLineOption nlcdOpt("nlcd-year", "Annual NLCD land-cover year (default 2023).", "year", "2023");
    QCommandLineOption ptOpt("make-points", "Write a single-point shapefile per site.");
    QCommandLineOption ptDirOpt("points-dir", "Base dir for point shapefiles (default: download dir, else output folder).", "dir");
    p.addOption(latOpt); p.addOption(lonOpt); p.addOption(idOpt);
    p.addOption(bufOpt); p.addOption(dlOpt); p.addOption(maxOpt); p.addOption(prodOpt);
    p.addOption(yearOpt);
    p.addOption(nlcdOpt);
    p.addOption(ptOpt); p.addOption(ptDirOpt);
    p.process(app);

    const QStringList args = p.positionalArguments();
    if (args.isEmpty()) p.showHelp(1);

    const QString inPath = args.at(0);
    const QString outPath = args.size() > 1 ? args.at(1)
                                            : (inPath.endsWith(".csv", Qt::CaseInsensitive)
                                                   ? inPath.left(inPath.size() - 4) + "_dem.csv" : inPath + "_dem.csv");

    // Select product types.
    QList<std::shared_ptr<ProductType>> products;
    const QStringList sel = p.value(prodOpt).toLower().split(',', Qt::SkipEmptyParts);
    const bool all = sel.contains("all");
    if (all || sel.contains("demhr") || sel.contains("dem"))
        products << std::make_shared<ElevationProduct>();
    if (all || sel.contains("demlr"))
        products << std::make_shared<ElevationLowResProduct>();
    if (all || sel.contains("hydro"))
        products << std::make_shared<HydrographyProduct>();
    if (all || sel.contains("roads"))
        products << std::make_shared<RoadsProduct>();
    if (all || sel.contains("landcover") || sel.contains("nlcd"))
        products << std::make_shared<LandCoverProduct>();

    if (products.isEmpty()) {
        QTextStream(stderr) << "No valid products selected. Use --products dem,demlr,hydro,roads,all.\n";
        return 2;
    }

    // TIGER/Line vintage for roads (defaults to 2025 if unset or invalid).
    const int tigerYear = p.value(yearOpt).toInt() > 0 ? p.value(yearOpt).toInt() : 2025;
    const int nlcdYear  = p.value(nlcdOpt).toInt() > 0 ? p.value(nlcdOpt).toInt() : 2023;

    ProcessOptions opts;
    opts.latColOverride = p.value(latOpt);
    opts.lonColOverride = p.value(lonOpt);
    opts.idColOverride  = p.value(idOpt);
    opts.bufferMeters   = p.value(bufOpt).toDouble();
    opts.downloadDir    = p.value(dlOpt);
    opts.maxTiles       = p.value(maxOpt).toInt();
    opts.makePoints     = p.isSet(ptOpt);
    opts.pointsDir      = p.value(ptDirOpt);

    CsvTable table;
    QString err;
    if (!table.load(inPath, &err)) {
        QTextStream(stderr) << err << "\n";
        return 2;
    }

    TnmClient client;
    TigerClient tiger(tigerYear);
    MrlcClient mrlc(nlcdYear);
    SiteProcessor processor(client, tiger, mrlc, products, opts);
    QTextStream log(stdout);
    if (!processor.run(table, outPath, log, &err)) {
        QTextStream(stderr) << err << "\n";
        return 2;
    }
    return 0;
}
