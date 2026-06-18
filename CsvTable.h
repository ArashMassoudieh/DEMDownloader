#pragma once
#include <QString>
#include <QStringList>
#include <QList>

// Reads a CSV with a header row, finds columns by candidate names, exposes
// rows, and writes an augmented CSV with appended columns. Handles quoted
// fields containing commas and escaped quotes.
class CsvTable {
public:
    bool load(const QString& path, QString* err);

    const QStringList& header() const { return m_header; }
    int rowCount() const { return m_rows.size(); }
    const QStringList& row(int i) const { return m_rows.at(i); }

    // Find a column index by trying override first (if non-empty), then a list
    // of case-insensitive candidate names. Returns -1 if not found.
    int findColumn(const QStringList& candidates, const QString& override) const;

    // Field value for a row, padded-safe (returns "" if out of range).
    QString field(int rowIdx, int colIdx) const;

    // CSV escaping for a single field.
    static QString escape(const QString& f);

    // Parse one CSV line into fields.
    static QStringList parseLine(const QString& line);

private:
    QStringList m_header;
    QList<QStringList> m_rows;
};
