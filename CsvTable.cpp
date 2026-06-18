#include "CsvTable.h"
#include <QtCore/QFile>
#include <QtCore/QTextStream>

QStringList CsvTable::parseLine(const QString& line) {
    QStringList out;
    QString cur;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
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

QString CsvTable::escape(const QString& f) {
    if (f.contains(',') || f.contains('"') || f.contains('\n')) {
        QString e = f; e.replace("\"", "\"\"");
        return "\"" + e + "\"";
    }
    return f;
}

bool CsvTable::load(const QString& path, QString* err) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = "cannot open input: " + path;
        return false;
    }
    QTextStream ts(&in);
    QStringList lines;
    while (!ts.atEnd()) lines << ts.readLine();
    in.close();
    if (lines.isEmpty()) {
        if (err) *err = "empty file";
        return false;
    }
    m_header = parseLine(lines.first());
    for (int li = 1; li < lines.size(); ++li) {
        if (lines.at(li).trimmed().isEmpty()) continue;
        QStringList fields = parseLine(lines.at(li));
        while (fields.size() < m_header.size()) fields << "";
        m_rows.append(fields);
    }
    return true;
}

int CsvTable::findColumn(const QStringList& candidates, const QString& override) const {
    if (!override.isEmpty()) {
        for (int i = 0; i < m_header.size(); ++i)
            if (m_header.at(i).trimmed().compare(override, Qt::CaseInsensitive) == 0) return i;
        return -1;
    }
    for (int i = 0; i < m_header.size(); ++i)
        for (const QString& c : candidates)
            if (m_header.at(i).trimmed().compare(c, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

QString CsvTable::field(int rowIdx, int colIdx) const {
    if (rowIdx < 0 || rowIdx >= m_rows.size()) return QString();
    const QStringList& r = m_rows.at(rowIdx);
    if (colIdx < 0 || colIdx >= r.size()) return QString();
    return r.at(colIdx);
}
