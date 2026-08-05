// Answers one question: when this app hashes an ISO, is it waiting on the disk
// or on the digest? The two call for opposite optimisations — unbuffered reads
// versus faster hashing — so the numbers below decide where tuning effort goes.
#include "core/checksum.h"
#include "core/verifier.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTextStream>

#include <algorithm>
#include <exception>
#include <limits>

#ifndef ISO_APP_VERSION
#define ISO_APP_VERSION "0.0.0-dev"
#endif

namespace {

constexpr qint64 SampleBlockSize = 8 * 1024 * 1024;
// Re-randomised per block so the sample stays incompressible even on a
// filesystem that compresses, which would otherwise flatter the read numbers.
constexpr qint64 SampleFreshBytesPerBlock = 64 * 1024;

struct Measurement {
    QString label;
    qint64 elapsedMs = 0;
};

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

double throughputMbPerSecond(qint64 bytes, qint64 elapsedMs)
{
    if (elapsedMs <= 0) {
        return 0.0;
    }
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (static_cast<double>(elapsedMs) / 1000.0);
}

void printRow(QTextStream& out, const QString& label, qint64 bytes, qint64 elapsedMs)
{
    out << QStringLiteral("  %1  %2 s  %3 MB/s")
               .arg(label, -32)
               .arg(static_cast<double>(elapsedMs) / 1000.0, 7, 'f', 2)
               .arg(throughputMbPerSecond(bytes, elapsedMs), 8, 'f', 1)
        << Qt::endl;
}

// Reports the fastest of the passes. Noise on a benchmark like this is
// one-sided — background I/O and scheduling only ever slow a run down — so the
// minimum is the closest estimate of what the machine can actually do.
template <typename Fn>
qint64 timeFastestPass(int passes, Fn&& runPass)
{
    qint64 fastest = std::numeric_limits<qint64>::max();
    for (int pass = 0; pass < passes; ++pass) {
        QElapsedTimer timer;
        timer.start();
        runPass();
        fastest = std::min(fastest, timer.elapsed());
    }
    return fastest;
}

QString generateSampleFile(qint64 sizeBytes, QTextStream& out)
{
    const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("iso-hash-bench-sample.bin"));
    out << QStringLiteral("Generating a %1 MiB sample at %2 ...").arg(sizeBytes / (1024 * 1024)).arg(path) << Qt::endl;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error(
            QStringLiteral("The sample file could not be created: %1").arg(file.errorString()).toStdString());
    }

    QByteArray block(SampleBlockSize, '\0');
    auto fillRandom = [&block](qint64 byteCount) {
        QRandomGenerator::global()->fillRange(
            reinterpret_cast<quint32*>(block.data()), static_cast<qsizetype>(byteCount / sizeof(quint32)));
    };
    fillRandom(SampleBlockSize);

    qint64 written = 0;
    while (written < sizeBytes) {
        fillRandom(SampleFreshBytesPerBlock);
        const qint64 chunk = std::min<qint64>(SampleBlockSize, sizeBytes - written);
        if (file.write(block.constData(), chunk) != chunk) {
            throw std::runtime_error(
                QStringLiteral("The sample file could not be written: %1").arg(file.errorString()).toStdString());
        }
        written += chunk;
    }

    if (!file.flush()) {
        throw std::runtime_error(
            QStringLiteral("The sample file could not be flushed: %1").arg(file.errorString()).toStdString());
    }
    return path;
}

void printVerdict(QTextStream& out, qint64 readOnlyMs, const QList<Measurement>& singleAlgorithmRuns)
{
    if (singleAlgorithmRuns.isEmpty() || readOnlyMs <= 0) {
        return;
    }

    const auto fastest = std::min_element(
        singleAlgorithmRuns.begin(), singleAlgorithmRuns.end(), [](const Measurement& a, const Measurement& b) {
            return a.elapsedMs < b.elapsedMs;
        });

    out << Qt::endl << QStringLiteral("Verdict") << Qt::endl;
    // A digest that keeps pace with the read pass is already free: the disk is
    // the wall, and only cheaper reads can move it.
    if (fastest->elapsedMs <= readOnlyMs * 11 / 10) {
        out << QStringLiteral("  I/O-bound. %1 finishes within 10% of a read-only pass, so the digest is "
                              "essentially free and the disk sets the pace.")
                   .arg(fastest->label)
            << Qt::endl
            << QStringLiteral("  Faster hashing would buy nothing here; unbuffered overlapped reads are the "
                              "only lever left.")
            << Qt::endl;
        return;
    }

    const double overhead =
        100.0 * (static_cast<double>(fastest->elapsedMs) - static_cast<double>(readOnlyMs)) / readOnlyMs;
    out << QStringLiteral("  Compute-bound. %1 takes %2% longer than a read-only pass, so the digest is the "
                          "bottleneck, not the disk.")
               .arg(fastest->label)
               .arg(overhead, 0, 'f', 0)
        << Qt::endl
        << QStringLiteral("  Cheaper reads would buy little here; keeping digests off each other's critical "
                          "path is what pays.")
        << Qt::endl;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("iso-hash-bench"));
    QCoreApplication::setApplicationVersion(QStringLiteral(ISO_APP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Measure whether ISO hashing is limited by disk reads or by the digest."));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption fileOption(
        QStringList{QStringLiteral("f"), QStringLiteral("file")},
        QStringLiteral("Benchmark an existing file instead of a generated sample."),
        QStringLiteral("path"));
    QCommandLineOption sizeOption(
        QStringList{QStringLiteral("s"), QStringLiteral("size")},
        QStringLiteral("Size of the generated sample in MiB."),
        QStringLiteral("mib"),
        QStringLiteral("2048"));
    QCommandLineOption passesOption(
        QStringList{QStringLiteral("p"), QStringLiteral("passes")},
        QStringLiteral("Passes per measurement; the fastest is reported."),
        QStringLiteral("count"),
        QStringLiteral("1"));
    QCommandLineOption algorithmsOption(
        QStringList{QStringLiteral("a"), QStringLiteral("algorithms")},
        QStringLiteral("Comma-separated algorithms to measure."),
        QStringLiteral("list"),
        QStringLiteral("SHA256,SHA512"));
    QCommandLineOption keepOption(
        QStringLiteral("keep"), QStringLiteral("Keep the generated sample file instead of deleting it."));

    parser.addOption(fileOption);
    parser.addOption(sizeOption);
    parser.addOption(passesOption);
    parser.addOption(algorithmsOption);
    parser.addOption(keepOption);
    parser.process(app);

    QTextStream out(stdout);
    QTextStream err(stderr);

    const QStringList algorithms = parseAlgorithmList(parser.value(algorithmsOption));
    if (algorithms.isEmpty()) {
        err << QStringLiteral("Error: no algorithms were requested.") << Qt::endl;
        return 2;
    }
    for (const QString& algorithm : algorithms) {
        if (!iso::supportedHashes().contains(algorithm)) {
            err << QStringLiteral("Error: unsupported hash algorithm: %1").arg(algorithm) << Qt::endl;
            return 2;
        }
    }

    bool sizeOk = false;
    const qint64 sampleMib = parser.value(sizeOption).toLongLong(&sizeOk);
    bool passesOk = false;
    const int passes = parser.value(passesOption).toInt(&passesOk);
    if (!sizeOk || sampleMib <= 0) {
        err << QStringLiteral("Error: --size must be a positive number of MiB.") << Qt::endl;
        return 2;
    }
    if (!passesOk || passes <= 0) {
        err << QStringLiteral("Error: --passes must be a positive count.") << Qt::endl;
        return 2;
    }

    QString filePath = parser.value(fileOption);
    const bool generated = filePath.isEmpty();

    try {
        if (generated) {
            filePath = generateSampleFile(sampleMib * 1024 * 1024, out);
        }

        const QFileInfo info(filePath);
        if (!info.exists() || !info.isFile()) {
            err << QStringLiteral("Error: %1 is not a readable file.").arg(filePath) << Qt::endl;
            return 2;
        }

        const qint64 fileSize = info.size();
        out << Qt::endl
            << QStringLiteral("File     %1").arg(info.absoluteFilePath()) << Qt::endl
            << QStringLiteral("Size     %1 MiB").arg(fileSize / (1024 * 1024)) << Qt::endl
            << QStringLiteral("Passes   %1 (fastest reported)").arg(passes) << Qt::endl
            << Qt::endl
            << QStringLiteral("Note: a file small enough to sit in the page cache measures RAM, not the disk.")
            << Qt::endl
            << QStringLiteral("      Use --size larger than free memory, or --file on a real ISO, for a true read "
                              "ceiling.")
            << Qt::endl
            << Qt::endl;

        const bool unbufferedAvailable = iso::bench::unbufferedIoAvailable(filePath);

        out << QStringLiteral("Read pipeline") << Qt::endl;
        const qint64 readOnlyMs = timeFastestPass(passes, [&]() { iso::bench::readOnlyPass(filePath, false); });
        printRow(out, QStringLiteral("read only, buffered"), fileSize, readOnlyMs);
        if (unbufferedAvailable) {
            const qint64 unbufferedReadMs =
                timeFastestPass(passes, [&]() { iso::bench::readOnlyPass(filePath, true); });
            printRow(out, QStringLiteral("read only, unbuffered"), fileSize, unbufferedReadMs);
        } else {
            out << QStringLiteral("  unbuffered reads are not available for this file") << Qt::endl;
        }

        out << Qt::endl << QStringLiteral("One algorithm per pass") << Qt::endl;
        QList<Measurement> nativeRuns;
        qint64 nativeRunTotalMs = 0;
        for (const QString& algorithm : algorithms) {
            const qint64 nativeMs =
                timeFastestPass(passes, [&]() { iso::bench::hashWithBackend(filePath, {algorithm}, true); });
            const qint64 qtMs =
                timeFastestPass(passes, [&]() { iso::bench::hashWithBackend(filePath, {algorithm}, false); });
            printRow(out, QStringLiteral("%1, native backend").arg(algorithm), fileSize, nativeMs);
            printRow(out, QStringLiteral("%1, Qt fallback").arg(algorithm), fileSize, qtMs);
            nativeRuns.append(Measurement{algorithm, nativeMs});
            nativeRunTotalMs += nativeMs;
        }

        if (unbufferedAvailable) {
            out << Qt::endl << QStringLiteral("Buffered versus unbuffered, native backend") << Qt::endl;
            for (const QString& algorithm : algorithms) {
                const qint64 unbufferedMs = timeFastestPass(
                    passes, [&]() { iso::bench::hashWithBackend(filePath, {algorithm}, true, true); });
                printRow(out, QStringLiteral("%1, unbuffered").arg(algorithm), fileSize, unbufferedMs);
            }
        }

        if (algorithms.size() > 1) {
            out << Qt::endl << QStringLiteral("All algorithms in one pass") << Qt::endl;
            const qint64 combinedMs =
                timeFastestPass(passes, [&]() { iso::bench::hashWithBackend(filePath, algorithms, true); });
            printRow(out, algorithms.join(QLatin1Char('+')), fileSize, combinedMs);
            out << QStringLiteral("  versus %1 s running them one after another (%2x)")
                       .arg(static_cast<double>(nativeRunTotalMs) / 1000.0, 0, 'f', 2)
                       .arg(
                           combinedMs > 0 ? static_cast<double>(nativeRunTotalMs) / static_cast<double>(combinedMs)
                                          : 0.0,
                           0,
                           'f',
                           2)
                << Qt::endl;
        }

        printVerdict(out, readOnlyMs, nativeRuns);

        if (generated && !parser.isSet(keepOption)) {
            QFile::remove(filePath);
        } else if (generated) {
            out << Qt::endl << QStringLiteral("Sample kept at %1").arg(filePath) << Qt::endl;
        }
    } catch (const std::exception& error) {
        err << QStringLiteral("Error: %1").arg(QString::fromUtf8(error.what())) << Qt::endl;
        if (generated && !parser.isSet(keepOption)) {
            QFile::remove(filePath);
        }
        return 2;
    }

    return 0;
}
