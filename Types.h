#pragma once
#include <QString>
#include <QList>
#include <QStringList>

// One resolution tier of a TNM product, e.g. "1 meter" DEM or
// "NHDPlus HR" flowlines. `dataset` is the exact TNMAccess "datasets" string.
struct Tier {
    QString label;
    QString dataset;
};

// One downloadable file returned by a TNMAccess query.
struct Tile {
    QString url;
    QString name;        // suggested base name (from item title), may be empty
    qint64  bytes = -1;  // -1 if unknown
};

// Result of querying a single tier at a single point.
struct QueryResult {
    bool ok = false;            // the query itself completed without error
    int tileCount = 0;          // number of matching products
    QString date;               // publication date of first item
    QString firstUrl;           // download URL of first item (for CSV summary)
    QList<Tile> tiles;          // all matching downloadable files
    QString error;              // populated when ok == false
};

// Outcome of resolving one product type for one site (after walking tiers).
struct ProductOutcome {
    QString bestResolution = "none";
    QString bestDataset;
    int tileCount = 0;
    QString date;
    QString firstUrl;
    QString status = "no coverage";
    int downloaded = 0;
    QString downloadDir;
    QList<Tile> tiles;          // tiles of the winning tier
};
