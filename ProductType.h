#pragma once
#include "Types.h"
#include <QString>
#include <QStringList>
#include <QList>
#include <memory>

// Abstract description of a TNM product category. A ProductType knows:
//  - its resolution tiers, ordered finest -> coarsest
//  - which download format(s) to request from TNMAccess
//  - a short key used for CSV column prefixes and subfolder names
//
// Adding a new USGS product (e.g. WBD watershed boundaries) means adding one
// subclass here; nothing else in the program needs to change.
class ProductType {
public:
    virtual ~ProductType() = default;

    // Short identifier, e.g. "dem" or "hydro". Used in column names
    // (dem_best_resolution) and download subfolders (<site>/dem/).
    virtual QString key() const = 0;

    // Human label for logs, e.g. "elevation (DEM)".
    virtual QString label() const = 0;

    // Resolution tiers, finest first. Walked in order; first hit wins.
    virtual const QList<Tier>& tiers() const = 0;

    // TNMAccess prodFormats value, e.g. "GeoTIFF" or "Shapefile".
    virtual QString prodFormats() const = 0;

    // Some products (DEM) come as many small tiles; others (NHD) come as one
    // large per-watershed package. This lets the processor log sensibly and
    // pick a default tile cap.
    virtual int defaultMaxTiles() const = 0;
};

// 3DEP elevation: 1 m -> 1/9 -> 1/3 -> 1 arc-second, GeoTIFF tiles.
class ElevationProduct : public ProductType {
public:
    QString key() const override { return "dem"; }
    QString label() const override { return "elevation (3DEP DEM)"; }
    const QList<Tier>& tiers() const override { return m_tiers; }
    QString prodFormats() const override { return "GeoTIFF"; }
    int defaultMaxTiles() const override { return 8; }
private:
    QList<Tier> m_tiers = {
        {"1 meter",        "Digital Elevation Model (DEM) 1 meter"},
        {"1/9 arc-second", "National Elevation Dataset (NED) 1/9 arc-second"},
        {"1/3 arc-second", "National Elevation Dataset (NED) 1/3 arc-second"},
        {"1 arc-second",   "National Elevation Dataset (NED) 1 arc-second"}
    };
};

// Hydrography flowlines: NHDPlus HR (highest res) -> NHD (best resolution).
// Delivered as zipped Shapefile packages by watershed unit, so typically one
// product per point. NHD was retired Oct 2023 but remains downloadable; it is
// kept as a fallback when NHDPlus HR has no coverage.
class HydrographyProduct : public ProductType {
public:
    QString key() const override { return "hydro"; }
    QString label() const override { return "hydrography (flowlines)"; }
    const QList<Tier>& tiers() const override { return m_tiers; }
    QString prodFormats() const override { return "Shapefile"; }
    int defaultMaxTiles() const override { return 4; }
private:
    QList<Tier> m_tiers = {
        {"NHDPlus HR", "National Hydrography Dataset Plus High Resolution (NHDPlus HR)"},
        {"NHD HR",     "National Hydrography Dataset (NHD) Best Resolution"}
    };
};
