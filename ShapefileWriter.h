#pragma once
#include <QString>
#include <QList>

// Writes a minimal single-point ESRI shapefile set (.shp/.shx/.dbf/.prj/.cpg)
// with no external GIS dependency. One point per file, with a small set of
// string/double attributes carried into the .dbf.
//
// Coordinates are WGS84 decimal degrees: X = longitude, Y = latitude. The .prj
// declares GCS_WGS_1984 so the result loads georeferenced in QGIS/ArcGIS.
class ShapefileWriter {
public:
    struct Attr {
        QString name;     // <=10 chars (dBASE field name limit)
        QString value;    // stored as text (field type 'C')
    };

    // Write <basePath>.shp/.shx/.dbf/.prj/.cpg for a single point.
    // basePath has no extension (e.g. ".../AZ12-301/point/AZ12-301").
    // Returns false (with *err set) on any I/O problem.
    static bool writePoint(const QString& basePath,
                           double lon, double lat,
                           const QList<Attr>& attrs,
                           QString* err);
};
