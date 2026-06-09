#pragma once

#include <QCryptographicHash>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace iso {

constexpr qint64 MaxChecksumFileSize = 1024 * 1024;

struct HashDetails {
    QString name;
    QCryptographicHash::Algorithm qtAlgorithm;
    qsizetype hexLength;
    bool legacy;
};

struct ParsedChecksum {
    QString algorithm;
    QString checksum;
    int lineNumber = 0;
    QString filename;
    QString rawLine;
};

const QHash<QString, HashDetails>& supportedHashes();
QStringList supportedHashNames();
QString normalizeChecksum(const QString& value);
QList<qsizetype> checksumMismatchPositions(const QString& expectedChecksum, const QString& computedChecksum);
QString formatChecksumMismatchSummary(const QList<qsizetype>& mismatchPositions, qsizetype maxListed = 8);
std::optional<QString> validateExpectedChecksum(const QString& expectedChecksum, const QString& algorithm);
std::optional<QString> algorithmFromChecksumLength(qsizetype length);
std::optional<ParsedChecksum> parseChecksumLine(const QString& line, int lineNumber);
ParsedChecksum parseChecksumText(const QString& text, const QString& isoPath = {});
ParsedChecksum
loadChecksumFile(const QString& checksumFilePath, const QString& isoPath = {}, qint64 maxSize = MaxChecksumFileSize);

} // namespace iso
