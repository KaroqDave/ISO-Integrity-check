#include "core/checksum.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <stdexcept>

namespace iso {
namespace {

const QRegularExpression HexPattern(QStringLiteral("^[0-9a-f]+$"));
const QRegularExpression ChecksumTokenPattern(QStringLiteral(
    "(?<![0-9a-fA-F])([0-9a-fA-F]{128}|[0-9a-fA-F]{64}|[0-9a-fA-F]{40}|[0-9a-fA-F]{32})(?![0-9a-fA-F])"));
const QRegularExpression BsdFilenamePattern(QStringLiteral("^[A-Za-z0-9-]+\\s*\\((.+)\\)\\s*=$"));

QString extractChecksumFilename(const QString& line, qsizetype tokenStart, qsizetype tokenEnd)
{
    const QString before = line.left(tokenStart).trimmed();
    const QString after = line.mid(tokenEnd).trimmed();

    const auto bsdMatch = BsdFilenamePattern.match(before);
    if (bsdMatch.hasMatch()) {
        return QString(bsdMatch.captured(1)).trimmed().remove(QRegularExpression(QStringLiteral("^\"|\"$")));
    }

    QString filename = after.isEmpty() ? before : after;
    if (filename.isEmpty()) {
        return {};
    }

    while (filename.startsWith(QLatin1Char('*'))) {
        filename.remove(0, 1);
    }
    filename = filename.trimmed();

    if (filename.startsWith(QLatin1Char('(')) && filename.contains(QLatin1Char(')'))) {
        filename = filename.mid(1, filename.indexOf(QLatin1Char(')')) - 1);
    }
    if (filename.startsWith(QLatin1Char('='))) {
        filename = filename.mid(1).trimmed();
    }

    return filename.trimmed().remove(QRegularExpression(QStringLiteral("^\"|\"$")));
}

bool checksumCandidateMatchesIso(const ParsedChecksum& candidate, const QString& isoName)
{
    if (candidate.filename.isEmpty()) {
        return false;
    }

    const QString normalizedPath = QString(candidate.filename).replace(QLatin1Char('\\'), QLatin1Char('/'));
    return QFileInfo(normalizedPath).fileName().toLower() == isoName;
}

ParsedChecksum parseChecksumLines(const QStringList& lines, const QString& isoPath)
{
    const QString isoName = isoPath.isEmpty() ? QString{} : QFileInfo(isoPath).fileName().toLower();
    std::optional<ParsedChecksum> firstCandidate;

    for (int index = 0; index < lines.size(); ++index) {
        auto parsed = parseChecksumLine(lines.at(index), index + 1);
        if (!parsed.has_value()) {
            continue;
        }

        if (!firstCandidate.has_value()) {
            firstCandidate = parsed;
        }

        if (isoName.isEmpty() || checksumCandidateMatchesIso(*parsed, isoName)) {
            return *parsed;
        }
    }

    if (firstCandidate.has_value()) {
        return *firstCandidate;
    }

    throw std::runtime_error("No supported checksum was found in the selected file.");
}

} // namespace

const QHash<QString, HashDetails>& supportedHashes()
{
    static const QHash<QString, HashDetails> hashes = {
        {QStringLiteral("SHA256"), {QStringLiteral("SHA256"), QCryptographicHash::Sha256, 64, false}},
        {QStringLiteral("SHA512"), {QStringLiteral("SHA512"), QCryptographicHash::Sha512, 128, false}},
        {QStringLiteral("SHA1"), {QStringLiteral("SHA1"), QCryptographicHash::Sha1, 40, true}},
        {QStringLiteral("MD5"), {QStringLiteral("MD5"), QCryptographicHash::Md5, 32, true}},
    };
    return hashes;
}

QStringList supportedHashNames()
{
    return {QStringLiteral("SHA256"), QStringLiteral("SHA512"), QStringLiteral("SHA1"), QStringLiteral("MD5")};
}

QString normalizeChecksum(const QString& value)
{
    return QString(value).trimmed().toLower();
}

std::optional<QString> validateExpectedChecksum(const QString& expectedChecksum, const QString& algorithm)
{
    const QString normalized = normalizeChecksum(expectedChecksum);
    if (normalized.isEmpty()) {
        return std::nullopt;
    }

    const auto hashes = supportedHashes();
    if (!hashes.contains(algorithm)) {
        return QStringLiteral("Unsupported hash algorithm: %1").arg(algorithm);
    }

    const qsizetype expectedLength = hashes.value(algorithm).hexLength;
    if (normalized.size() != expectedLength) {
        return QStringLiteral("%1 checksums must be %2 hexadecimal characters. The pasted value has %3.")
            .arg(algorithm)
            .arg(expectedLength)
            .arg(normalized.size());
    }

    if (!HexPattern.match(normalized).hasMatch()) {
        return QStringLiteral("%1 checksums can only contain hexadecimal characters.").arg(algorithm);
    }

    return std::nullopt;
}

std::optional<QString> algorithmFromChecksumLength(qsizetype length)
{
    for (const auto& algorithm : supportedHashNames()) {
        if (supportedHashes().value(algorithm).hexLength == length) {
            return algorithm;
        }
    }
    return std::nullopt;
}

std::optional<ParsedChecksum> parseChecksumLine(const QString& line, int lineNumber)
{
    const QString stripped = QString(line).trimmed();
    if (stripped.isEmpty() || stripped.startsWith(QLatin1Char('#')) || stripped.startsWith(QLatin1Char(';'))) {
        return std::nullopt;
    }

    const auto match = ChecksumTokenPattern.match(stripped);
    if (!match.hasMatch()) {
        return std::nullopt;
    }

    const QString checksum = match.captured(1).toLower();
    const auto algorithm = algorithmFromChecksumLength(checksum.size());
    if (!algorithm.has_value()) {
        return std::nullopt;
    }

    return ParsedChecksum{
        *algorithm,
        checksum,
        lineNumber,
        extractChecksumFilename(stripped, match.capturedStart(1), match.capturedEnd(1)),
        stripped,
    };
}

ParsedChecksum parseChecksumText(const QString& text, const QString& isoPath)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return parseChecksumLines(normalized.split(QLatin1Char('\n')), isoPath);
}

ParsedChecksum loadChecksumFile(const QString& checksumFilePath, const QString& isoPath, qint64 maxSize)
{
    const QFileInfo info(checksumFilePath);
    if (!info.exists()) {
        throw std::runtime_error("The selected checksum file does not exist.");
    }
    if (!info.isFile()) {
        throw std::runtime_error("The selected checksum path is not a file.");
    }
    if (info.size() > maxSize) {
        throw std::runtime_error(
            QStringLiteral("The selected checksum file is too large. Choose a checksum text file under %1 KB.")
                .arg(maxSize / 1024)
                .toStdString());
    }

    QFile file(checksumFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(
            QStringLiteral("The selected checksum file could not be opened: %1").arg(file.errorString()).toStdString());
    }

    QByteArray data = file.readAll();
    if (data.startsWith("\xEF\xBB\xBF")) {
        data.remove(0, 3);
    }

    return parseChecksumText(QString::fromUtf8(data), isoPath);
}

} // namespace iso
