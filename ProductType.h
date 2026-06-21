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
    // Short identifier, e.g. "demhr" or "hydro". Used in column names
    // (demhr_best_resolution) and download subfolders (<site>/demhr/).
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
    // Most products are queried by bounding box through TNMAccess. A few
    // (roads) are not available that way and are fetched per-county from
    // Census TIGER/Line instead. The processor checks this to route them.
    virtual bool isCountyBased() const { return false; }
};
// 3DEP elevation, HIGH RESOLUTION: best available, 1 m -> 1/9 -> 1/3 -> 1 arc-sec.
// Folder: <site>/demhr/   Use for HEC-RAS (fine terrain).
class ElevationProduct : public ProductType {
public:
    QString key() const override { return "demhr"; }
    QString label() const override { return "elevation high-res (3DEP DEM)"; }
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
// 3DEP elevation, LOW RESOLUTION: 1/3 arc-second only (~10 m).
// Folder: <site>/demlr/   Use for HEC-HMS (coarser terrain, smaller files).
class ElevationLowResProduct : public ProductType {
public:
    QString key() const override { return "demlr"; }
    QString label() const override { return "elevation low-res (1/3 arc-sec)"; }
    const QList<Tier>& tiers() const override { return m_tiers; }
    QString prodFormats() const override { return "GeoTIFF"; }
    int defaultMaxTiles() const override { return 8; }
private:
    QList<Tier> m_tiers = {
        {"1/3 arc-second", "National Elevation Dataset (NED) 1/3 arc-second"}
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

// Transportation / roads: Census TIGER/Line "All Roads", fetched PER COUNTY
// (not by bbox). The USGS NTD is unusable through TNMAccess (its product query
// errors server-side), so roads come straight from the Census Bureau. Because
// this product is county-based, isCountyBased() returns true and the processor
// routes it to TigerClient rather than TnmClient. The tiers() list is unused
// for county-based products but kept non-empty to satisfy the interface.
// Folder: <site>/roads/
class RoadsProduct : public ProductType {
public:
    QString key() const override { return "roads"; }
    QString label() const override { return "transportation (roads, TIGER/Line)"; }
    const QList<Tier>& tiers() const override { return m_tiers; }
    QString prodFormats() const override { return "Shapefile"; }
    int defaultMaxTiles() const override { return 1; }   // one county package
    bool isCountyBased() const override { return true; }
private:
    QList<Tier> m_tiers = {
        {"All Roads", "Census TIGER/Line All Roads (county)"}
    };
};
