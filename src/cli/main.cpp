#include "core/checksum.h"
#include "core/verifier.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>

namespace {

struct CliOptions {
    QString filePath;
    QString expectedChecksum;
    QString checksumFilePath;
    QString algorithm = QStringLiteral("SHA256");
    bool hashOnly = false;
};

int exitCodeForStatus(iso::VerificationStatus status)
{
    switch (status) {
    case iso::VerificationStatus::Match:
    case iso::VerificationStatus::Generated:
        return 0;
    case iso::VerificationStatus::Mismatch:
        return 1;
    case iso::VerificationStatus::Error:
    case iso::VerificationStatus::Cancelled:
        return 2;
    }
    return 2;
}

CliOptions parseOptions(QCoreApplication& app)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Verify ISO file integrity using trusted checksums."));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption fileOption(QStringList{QStringLiteral("f"), QStringLiteral("file")}, QStringLiteral("ISO file to verify."), QStringLiteral("path"));
    QCommandLineOption expectedOption(QStringList{QStringLiteral("e"), QStringLiteral("expected")}, QStringLiteral("Expected checksum value."), QStringLiteral("hash"));
    QCommandLineOption checksumFileOption(QStringList{QStringLiteral("c"), QStringLiteral("checksum-file")}, QStringLiteral("Checksum file to import."), QStringLiteral("path"));
    QCommandLineOption algorithmOption(QStringList{QStringLiteral("a"), QStringLiteral("algorithm")}, QStringLiteral("Hash algorithm (SHA256, SHA512, SHA1, MD5)."), QStringLiteral("name"), QStringLiteral("SHA256"));

    parser.addOption(fileOption);
    parser.addOption(expectedOption);
    parser.addOption(checksumFileOption);
    parser.addOption(algorithmOption);
    parser.process(app);

    CliOptions options;
    options.filePath = parser.value(fileOption);
    options.expectedChecksum = parser.value(expectedOption);
    options.checksumFilePath = parser.value(checksumFileOption);
    options.algorithm = parser.value(algorithmOption).toUpper();
    options.hashOnly = !parser.isSet(expectedOption) && !parser.isSet(checksumFileOption);
    return options;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("iso-integrity-check-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.1.0"));

    const CliOptions options = parseOptions(app);
    if (options.filePath.isEmpty()) {
        QTextStream(stderr) << "Choose an ISO file with --file.\n";
        return 2;
    }

    QString algorithm = options.algorithm;
    QString expectedChecksum = options.expectedChecksum;

    if (!options.checksumFilePath.isEmpty()) {
        try {
            const auto parsed = iso::loadChecksumFile(options.checksumFilePath, options.filePath);
            algorithm = parsed.algorithm;
            expectedChecksum = parsed.checksum;
        } catch (const std::exception& error) {
            QTextStream(stderr) << error.what() << '\n';
            return 2;
        }
    } else if (expectedChecksum.isEmpty()) {
        if (const auto validationError = iso::validateExpectedChecksum({}, algorithm); validationError.has_value()) {
            Q_UNUSED(validationError);
        }
    } else if (const auto inferred = iso::algorithmFromChecksumLength(iso::normalizeChecksum(expectedChecksum).size())) {
        if (options.algorithm == QStringLiteral("SHA256") && algorithm != *inferred) {
            algorithm = *inferred;
        }
    }

    const auto result = iso::verifyChecksum(options.filePath, expectedChecksum, algorithm);
    QTextStream out(stdout);
    out << "Algorithm: " << algorithm << '\n';
    if (!result.computedHash.isEmpty()) {
        out << "Computed: " << result.computedHash << '\n';
    }

    switch (result.status) {
    case iso::VerificationStatus::Match:
        out << "MATCH: " << result.message << '\n';
        break;
    case iso::VerificationStatus::Mismatch:
        out << "MISMATCH: " << result.message << '\n';
        break;
    case iso::VerificationStatus::Generated:
        out << result.message << '\n';
        break;
    case iso::VerificationStatus::Cancelled:
        out << "CANCELLED: " << result.message << '\n';
        break;
    case iso::VerificationStatus::Error:
        QTextStream(stderr) << result.message << '\n';
        break;
    }

    return exitCodeForStatus(result.status);
}
