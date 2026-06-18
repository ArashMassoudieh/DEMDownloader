#include "ShapefileWriter.h"
#include <QtCore/QFile>
#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QtEndian>
#include <QtCore/QDate>

namespace {

// Append a big-endian 32-bit int to a byte array.
void putBE32(QByteArray& b, qint32 v) {
    char buf[4];
    qToBigEndian<qint32>(v, buf);
    b.append(buf, 4);
}
// Append a little-endian 32-bit int.
void putLE32(QByteArray& b, qint32 v) {
    char buf[4];
    qToLittleEndian<qint32>(v, buf);
    b.append(buf, 4);
}
// Append a little-endian IEEE-754 double.
void putLEd(QByteArray& b, double v) {
    char buf[8];
    qToLittleEndian<double>(v, buf);
    b.append(buf, 8);
}

bool writeFile(const QString& path, const QByteArray& data, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = "cannot write: " + path;
        return false;
    }
    f.write(data);
    f.close();
    return true;
}

// 100-byte .shp/.shx header. fileLenWords = total file length in 16-bit words.
QByteArray makeHeader(qint32 fileLenWords, double lon, double lat) {
    QByteArray h;
    putBE32(h, 0x0000270a);              // file code
    for (int i = 0; i < 5; ++i) putBE32(h, 0); // unused (5 ints)
    putBE32(h, fileLenWords);            // file length (16-bit words)
    putLE32(h, 1000);                    // version
    putLE32(h, 1);                       // shape type 1 = Point
    // Bounding box (Xmin,Ymin,Xmax,Ymax) — degenerate for a single point.
    putLEd(h, lon); putLEd(h, lat);
    putLEd(h, lon); putLEd(h, lat);
    // Zmin,Zmax,Mmin,Mmax — unused for 2D point.
    putLEd(h, 0.0); putLEd(h, 0.0);
    putLEd(h, 0.0); putLEd(h, 0.0);
    return h; // exactly 100 bytes
}

} // namespace

bool ShapefileWriter::writePoint(const QString& basePath,
                                 double lon, double lat,
                                 const QList<Attr>& attrs,
                                 QString* err) {
    // ---- .shp ------------------------------------------------------------
    // One Point record: record header (8 bytes) + content (4 + 8 + 8 = 20).
    // Content length is in 16-bit words: 20 bytes / 2 = 10 words.
    // File length = 100-byte header + 8-byte rec header + 20-byte content
    //             = 128 bytes = 64 words.
    {
        QByteArray shp = makeHeader(64, lon, lat);
        // record header: record number (BE), content length in words (BE)
        putBE32(shp, 1);     // record number (1-based)
        putBE32(shp, 10);    // content length = 10 words
        // record content: shape type (LE) + X + Y (LE doubles)
        putLE32(shp, 1);     // Point
        putLEd(shp, lon);    // X = longitude
        putLEd(shp, lat);    // Y = latitude
        if (!writeFile(basePath + ".shp", shp, err)) return false;
    }

    // ---- .shx ------------------------------------------------------------
    // Header (50 words) + one 8-byte record (4 words). File length = 54 words.
    {
        QByteArray shx = makeHeader(54, lon, lat);
        // record: offset to .shp record header (in words), content length (words)
        putBE32(shx, 50);    // first record starts right after 100-byte header
        putBE32(shx, 10);    // content length = 10 words
        if (!writeFile(basePath + ".shx", shx, err)) return false;
    }

    // ---- .dbf (dBASE III) ------------------------------------------------
    // All attributes stored as fixed-width character (type 'C') fields.
    {
        const int nFields = attrs.size();
        const int headerLen = 32 + 32 * nFields + 1; // file hdr + field descrs + terminator
        QList<int> widths;
        int recLen = 1; // 1 byte deletion flag
        for (const Attr& a : attrs) {
            int w = a.value.toUtf8().size();
            if (w < 1) w = 1;
            if (w > 254) w = 254;
            widths << w;
            recLen += w;
        }

        QByteArray dbf;
        dbf.append(char(0x03)); // version: dBASE III without memo
        QDate today = QDate::currentDate();
        dbf.append(char(today.year() - 1900));
        dbf.append(char(today.month()));
        dbf.append(char(today.day()));
        putLE32(dbf, 1);                 // number of records
        // header length (LE 16-bit) and record length (LE 16-bit)
        dbf.append(char(headerLen & 0xFF)); dbf.append(char((headerLen >> 8) & 0xFF));
        dbf.append(char(recLen & 0xFF));    dbf.append(char((recLen >> 8) & 0xFF));
        for (int i = 0; i < 20; ++i) dbf.append(char(0)); // reserved

        // field descriptors (32 bytes each)
        for (int i = 0; i < nFields; ++i) {
            QByteArray name = attrs.at(i).name.left(10).toUtf8();
            for (int k = 0; k < 11; ++k)
                dbf.append(k < name.size() ? name.at(k) : char(0)); // 11-byte name, null-padded
            dbf.append('C');                              // field type: Character
            putLE32(dbf, 0);                              // field data address (ignored)
            dbf.append(char(widths.at(i)));               // field length
            dbf.append(char(0));                          // decimal count
            for (int k = 0; k < 14; ++k) dbf.append(char(0)); // reserved
        }
        dbf.append(char(0x0D)); // header terminator

        // one record: deletion flag (space) + space-padded fields
        dbf.append(' ');
        for (int i = 0; i < nFields; ++i) {
            QByteArray v = attrs.at(i).value.toUtf8().left(widths.at(i));
            dbf.append(v);
            for (int k = v.size(); k < widths.at(i); ++k) dbf.append(' ');
        }
        dbf.append(char(0x1A)); // EOF marker
        if (!writeFile(basePath + ".dbf", dbf, err)) return false;
    }

    // ---- .prj (WGS84 geographic) ----------------------------------------
    {
        const char* wkt =
            "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\","
            "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
            "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]]";
        if (!writeFile(basePath + ".prj", QByteArray(wkt), err)) return false;
    }

    // ---- .cpg (encoding hint) -------------------------------------------
    if (!writeFile(basePath + ".cpg", QByteArray("UTF-8"), err)) return false;

    return true;
}
