#pragma once

#include <QCryptographicHash>
#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

namespace iso {

constexpr qsizetype ChunkSize = 4 * 1024 * 1024;
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
std::optional<QString> validateExpectedChecksum(const QString& expectedChecksum, const QString& algorithm);
std::optional<QString> algorithmFromChecksumLength(qsizetype length);
std::optional<ParsedChecksum> parseChecksumLine(const QString& line, int lineNumber);
ParsedChecksum parseChecksumText(const QString& text, const QString& isoPath = {});
ParsedChecksum loadChecksumFile(const QString& checksumFilePath, const QString& isoPath = {}, qint64 maxSize = MaxChecksumFileSize);

} // namespace iso
