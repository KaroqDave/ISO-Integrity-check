#include "core/checksum.h"
#include "core/verifier.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#ifndef ISO_APP_VERSION
#define ISO_APP_VERSION "0.0.0-dev"
#endif

namespace {

struct CliOptions {
    QString filePath;
    QString expectedChecksum;
    QString checksumFilePath;
    QStringList algorithms;
    bool algorithmExplicit = false;
    bool hashOnly = false;
    iso::IoPolicy ioPolicy = iso::IoPolicy::Buffered;
};

// Accepts "SHA256" or "SHA256,SHA512". Order is preserved and duplicates dropped
// so the first entry stays a predictable default for verification.
QStringList parseAlgorithmList(const QString& value)
{
    QStringList algorithms;
    for (const QString& part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString name = part.trimmed().toUpper();
        if (!name.isEmpty() && !algorithms.contains(name)) {
            algorithms.append(name);
        }
    }
    return algorithms;
}

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

    QCommandLineOption fileOption(
        QStringList{QStringLiteral("f"), QStringLiteral("file")},
        QStringLiteral("ISO file to verify."),
        QStringLiteral("path"));
    QCommandLineOption expectedOption(
        QStringList{QStringLiteral("e"), QStringLiteral("expected")},
        QStringLiteral("Expected checksum value."),
        QStringLiteral("hash"));
    QCommandLineOption checksumFileOption(
        QStringList{QStringLiteral("c"), QStringLiteral("checksum-file")},
        QStringLiteral("Checksum file to import."),
        QStringLiteral("path"));
    QCommandLineOption algorithmOption(
        QStringList{QStringLiteral("a"), QStringLiteral("algorithm")},
        QStringLiteral("Hash algorithm, or a comma-separated list computed in a single read pass "
                       "(SHA256, SHA512, SHA1, MD5)."),
        QStringLiteral("names"),
        QStringLiteral("SHA256"));
    QCommandLineOption allOption(
        QStringList{QStringLiteral("A"), QStringLiteral("all")},
        QStringLiteral("Compute every supported algorithm in a single read pass."));
    QCommandLineOption unbufferedOption(
        QStringLiteral("unbuffered"),
        QStringLiteral("Read past the operating system's file cache instead of through it. Leaves the cache "
                       "intact for the rest of the system, at the cost of speed when the file is already "
                       "cached."));

    parser.addOption(fileOption);
    parser.addOption(expectedOption);
    parser.addOption(checksumFileOption);
    parser.addOption(algorithmOption);
    parser.addOption(allOption);
    parser.addOption(unbufferedOption);
    parser.process(app);

    CliOptions options;
    options.filePath = parser.value(fileOption);
    options.expectedChecksum = parser.value(expectedOption);
    options.checksumFilePath = parser.value(checksumFileOption);
    options.algorithms =
        parser.isSet(allOption) ? iso::supportedHashNames() : parseAlgorithmList(parser.value(algorithmOption));
    options.algorithmExplicit = parser.isSet(allOption) || parser.isSet(algorithmOption);
    options.hashOnly = !parser.isSet(expectedOption) && !parser.isSet(checksumFileOption);
    options.ioPolicy = parser.isSet(unbufferedOption) ? iso::IoPolicy::Unbuffered : iso::IoPolicy::Buffered;
    return options;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("iso-integrity-check-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(ISO_APP_VERSION));

    const CliOptions options = parseOptions(app);
    if (options.filePath.isEmpty()) {
        QTextStream(stderr) << "Choose an ISO file with --file.\n";
        return 2;
    }

    QStringList algorithms = options.algorithms;
    if (algorithms.isEmpty()) {
        QTextStream(stderr) << "No hash algorithm was given to --algorithm.\n";
        return 2;
    }

    // Reject typos up front. verifyChecksum silently ignores unknown names in its
    // extras list, which would quietly drop "SHA51" from --algorithm SHA256,SHA51.
    for (const QString& name : algorithms) {
        if (!iso::supportedHashes().contains(name)) {
            QTextStream(stderr) << "Unsupported hash algorithm: " << name << ". Supported: "
                                << iso::supportedHashNames().join(QStringLiteral(", ")) << ".\n";
            return 2;
        }
    }

    QString algorithm = algorithms.first();
    QString expectedChecksum = options.expectedChecksum;

    if (!options.checksumFilePath.isEmpty()) {
        try {
            const auto parsed = iso::loadChecksumFile(options.checksumFilePath, options.filePath);
            algorithm = parsed.algorithm;
            expectedChecksum = parsed.checksum;
            if (!algorithms.contains(algorithm)) {
                algorithms.prepend(algorithm);
            }
        } catch (const std::exception& error) {
            QTextStream(stderr) << error.what() << '\n';
            return 2;
        }
    } else if (
        const auto inferred = iso::algorithmFromChecksumLength(iso::normalizeChecksum(expectedChecksum).size())) {
        if (algorithms.contains(*inferred)) {
            // Several algorithms were asked for and one matches the pasted
            // checksum's length; verify against that one.
            algorithm = *inferred;
        } else if (!options.algorithmExplicit) {
            // Auto-detect the algorithm from a pasted checksum's length when the
            // user left the default (SHA256) selected.
            algorithm = *inferred;
            algorithms = {*inferred};
        }
    }

    const QStringList alsoCompute = algorithms.mid(algorithms.indexOf(algorithm) + 1) +
                                    algorithms.mid(0, algorithms.indexOf(algorithm));
    const auto result =
        iso::verifyChecksum(options.filePath, expectedChecksum, algorithm, {}, {}, alsoCompute, options.ioPolicy);

    QTextStream out(stdout);
    if (algorithms.size() == 1) {
        out << "Algorithm: " << algorithm << '\n';
        if (!result.computedHash.isEmpty()) {
            out << "Computed: " << result.computedHash << '\n';
        }
    } else {
        for (const QString& name : algorithms) {
            const QString computed = result.computedHashes.value(name);
            if (!computed.isEmpty()) {
                out << name << ": " << computed << '\n';
            }
        }
        if (!options.hashOnly && !result.computedHash.isEmpty()) {
            out << "Verified against: " << algorithm << '\n';
        }
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
