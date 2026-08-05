#include "core/checksum.h"
#include "core/verifier.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace iso;

class ChecksumTests : public QObject {
    Q_OBJECT

  private slots:
    void calculatesAllSupportedAlgorithms();
    void expectedHashIsCaseInsensitiveAndTrimmed();
    void invalidChecksumLengthRejected();
    void invalidChecksumCharactersRejected();
    void mismatchPositionsIncludeAllDifferences();
    void mismatchSummaryListsDifferences();
    void sha1ValidationUses40HexCharacters();
    void plainSha256FileIsParsed();
    void gnuStyleLineWithFilenameIsParsed();
    void gnuBinaryMarkerFilenameIsParsed();
    void matchingIsoFilenameWinsOverFirstSupportedHash();
    void exactIsoFilenameWinsOverPartialFilenameMatch();
    void firstSupportedHashUsedWithoutFilenameMatch();
    void unsupportedOrMalformedFileIsRejected();
    void loadChecksumFileReadsAndParsesFile();
    void loadChecksumFileRejectsOversizedFiles();
    void bsdStyleLineWithFilenameIsParsed();
    void matchingBsdStyleFilenameWinsOverFirstSupportedHash();
    void checksumFileWithBomAndCrlfIsParsed();
    void uppercaseChecksumIsNormalizedToLowercase();
    void commentAndBlankLinesAreSkipped();
};

class VerifierTests : public QObject {
    Q_OBJECT

  private slots:
    void mismatchIsReported();
    void missingChecksumGeneratesHash();
    void missingFileIsReported();
    void parsedChecksumCanVerifyMatch();
    void parsedChecksumCanVerifyMismatch();
    void cancellationStopsVerification();
    void cancellationMidRunStopsParallelDigests();
    void unbufferedReadsMatchBufferedDigest();
    void unbufferedReadsHandleSmallAndEmptyFiles();
    void multiChunkFileHashMatchesReference();
    void emptyFileHashesToTheEmptyDigest();
    void directoryPathIsReportedAsError();
    void singlePassComputesEveryRequestedAlgorithm();
    void singlePassAgreesWithSequentialHashingAcrossChunks();
    void duplicateAlgorithmsAreCollapsed();
    void unsupportedAlgorithmInListIsRejected();
    void emptyAlgorithmListIsRejected();
    void alsoComputeReturnsExtraHashesWithoutAffectingTheVerdict();
    void singlePassReportsProgressOncePerChunk();
};

void ChecksumTests::calculatesAllSupportedAlgorithms()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(file.write(data) == data.size());
    file.close();

    QCryptographicHash sha256(QCryptographicHash::Sha256);
    sha256.addData(data);
    QCryptographicHash sha512(QCryptographicHash::Sha512);
    sha512.addData(data);
    QCryptographicHash sha1(QCryptographicHash::Sha1);
    sha1.addData(data);
    QCryptographicHash md5(QCryptographicHash::Md5);
    md5.addData(data);

    QCOMPARE(calculateFileHash(filePath, QStringLiteral("SHA256")), QString::fromLatin1(sha256.result().toHex()));
    QCOMPARE(calculateFileHash(filePath, QStringLiteral("SHA512")), QString::fromLatin1(sha512.result().toHex()));
    QCOMPARE(calculateFileHash(filePath, QStringLiteral("SHA1")), QString::fromLatin1(sha1.result().toHex()));
    QCOMPARE(calculateFileHash(filePath, QStringLiteral("MD5")), QString::fromLatin1(md5.result().toHex()));
}

void ChecksumTests::expectedHashIsCaseInsensitiveAndTrimmed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(file.write(data) == data.size());
    file.close();

    const QString expected =
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()).toUpper();

    const auto result = verifyChecksum(filePath, QStringLiteral("  %1\n").arg(expected), QStringLiteral("SHA256"));
    QCOMPARE(result.status, VerificationStatus::Match);
    QVERIFY(result.matches.has_value());
    QVERIFY(*result.matches);
}

void ChecksumTests::invalidChecksumLengthRejected()
{
    const auto error = validateExpectedChecksum(QStringLiteral("abc"), QStringLiteral("SHA256"));
    QVERIFY(error.has_value());
    QVERIFY(error->contains(QStringLiteral("64 hexadecimal characters")));
}

void ChecksumTests::invalidChecksumCharactersRejected()
{
    const auto error = validateExpectedChecksum(QString(64, QLatin1Char('g')), QStringLiteral("SHA256"));
    QVERIFY(error.has_value());
    QVERIFY(error->contains(QStringLiteral("hexadecimal")));
}

void ChecksumTests::mismatchPositionsIncludeAllDifferences()
{
    const auto positions = checksumMismatchPositions(QStringLiteral("  aabbccdd\n"), QStringLiteral("aab0cceeff"));

    QCOMPARE(positions, QList<qsizetype>({3, 6, 7, 8, 9}));
}

void ChecksumTests::mismatchSummaryListsDifferences()
{
    const auto summary = formatChecksumMismatchSummary(QList<qsizetype>({0, 2, 4}), 2);

    QCOMPARE(summary, QStringLiteral("3 differences at characters 1, 3 and 1 more (1-based)."));
}

void ChecksumTests::sha1ValidationUses40HexCharacters()
{
    const auto error = validateExpectedChecksum(QString(39, QLatin1Char('a')), QStringLiteral("SHA1"));
    QVERIFY(error.has_value());
    QVERIFY(error->contains(QStringLiteral("40 hexadecimal characters")));
}

void ChecksumTests::plainSha256FileIsParsed()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile iso(isoPath);
    QVERIFY(iso.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(iso.write(data) == data.size());
    iso.close();

    const QString checksum = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    const auto parsed = parseChecksumText(checksum, isoPath);

    QCOMPARE(parsed.algorithm, QStringLiteral("SHA256"));
    QCOMPARE(parsed.checksum, checksum);
    QCOMPARE(parsed.lineNumber, 1);
}

void ChecksumTests::gnuStyleLineWithFilenameIsParsed()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile iso(isoPath);
    QVERIFY(iso.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(iso.write(data) == data.size());
    iso.close();

    const QString checksum = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex());
    const auto parsed = parseChecksumText(QStringLiteral("%1  sample.iso").arg(checksum), isoPath);

    QCOMPARE(parsed.algorithm, QStringLiteral("SHA512"));
    QCOMPARE(parsed.checksum, checksum);
    QCOMPARE(parsed.filename, QStringLiteral("sample.iso"));
}

void ChecksumTests::gnuBinaryMarkerFilenameIsParsed()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile iso(isoPath);
    QVERIFY(iso.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(iso.write(data) == data.size());
    iso.close();

    const QString checksum = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
    const auto parsed = parseChecksumText(QStringLiteral("%1 *sample.iso").arg(checksum), isoPath);

    QCOMPARE(parsed.algorithm, QStringLiteral("MD5"));
    QCOMPARE(parsed.filename, QStringLiteral("sample.iso"));
}

void ChecksumTests::matchingIsoFilenameWinsOverFirstSupportedHash()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile iso(isoPath);
    QVERIFY(iso.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(iso.write(data) == data.size());
    iso.close();

    const QString matchingHash =
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    const QString text =
        QStringLiteral("%1  other.iso\n%2  sample.iso\n").arg(QString(64, QLatin1Char('0')), matchingHash);
    const auto parsed = parseChecksumText(text, isoPath);

    QCOMPARE(parsed.checksum, matchingHash);
    QCOMPARE(parsed.filename, QStringLiteral("sample.iso"));
}

void ChecksumTests::exactIsoFilenameWinsOverPartialFilenameMatch()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile iso(isoPath);
    QVERIFY(iso.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(iso.write(data) == data.size());
    iso.close();

    const QString matchingHash =
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    const QString text =
        QStringLiteral("%1  sample.iso.zsync\n%2  sample.iso\n").arg(QString(64, QLatin1Char('0')), matchingHash);
    const auto parsed = parseChecksumText(text, isoPath);

    QCOMPARE(parsed.checksum, matchingHash);
    QCOMPARE(parsed.filename, QStringLiteral("sample.iso"));
}

void ChecksumTests::firstSupportedHashUsedWithoutFilenameMatch()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    const QString firstHash = QString(40, QLatin1Char('1'));
    const QString secondHash = QString(64, QLatin1Char('2'));
    const QString text = QStringLiteral("%1  other.iso\n%2  another.iso\n").arg(firstHash, secondHash);

    const auto parsed = parseChecksumText(text, isoPath);
    QCOMPARE(parsed.algorithm, QStringLiteral("SHA1"));
    QCOMPARE(parsed.checksum, firstHash);
}

void ChecksumTests::unsupportedOrMalformedFileIsRejected()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");

    bool threw = false;
    try {
        parseChecksumText(QStringLiteral("not a checksum\nalso not a checksum"), isoPath);
    } catch (const std::exception&) {
        threw = true;
    }
    QVERIFY(threw);
}

void ChecksumTests::loadChecksumFileReadsAndParsesFile()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile iso(isoPath);
    QVERIFY(iso.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(iso.write(data) == data.size());
    iso.close();

    const QString checksum = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
    const QString checksumFilePath = tempDir.path() + QStringLiteral("/sample.sha1");
    QFile checksumFile(checksumFilePath);
    QVERIFY(checksumFile.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(checksumFile.write(QStringLiteral("%1  sample.iso\n").arg(checksum).toUtf8()) > 0);
    checksumFile.close();

    const auto parsed = loadChecksumFile(checksumFilePath, isoPath);
    QCOMPARE(parsed.algorithm, QStringLiteral("SHA1"));
    QCOMPARE(parsed.checksum, checksum);
}

void ChecksumTests::loadChecksumFileRejectsOversizedFiles()
{
    QTemporaryDir tempDir;
    const QString checksumFilePath = tempDir.path() + QStringLiteral("/large.sha256");
    QFile checksumFile(checksumFilePath);
    QVERIFY(checksumFile.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(checksumFile.write(QString(64, QLatin1Char('0')).toUtf8()) > 0);
    checksumFile.close();

    bool threw = false;
    try {
        loadChecksumFile(checksumFilePath, {}, 32);
    } catch (const std::exception& error) {
        threw = true;
        QVERIFY(QString::fromUtf8(error.what()).contains(QStringLiteral("too large")));
    }
    QVERIFY(threw);
}

void ChecksumTests::bsdStyleLineWithFilenameIsParsed()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile iso(isoPath);
    QVERIFY(iso.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(iso.write(data) == data.size());
    iso.close();

    const QString checksum = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    const auto parsed = parseChecksumText(QStringLiteral("SHA256 (sample.iso) = %1").arg(checksum), isoPath);

    QCOMPARE(parsed.algorithm, QStringLiteral("SHA256"));
    QCOMPARE(parsed.checksum, checksum);
    QCOMPARE(parsed.filename, QStringLiteral("sample.iso"));
}

void ChecksumTests::matchingBsdStyleFilenameWinsOverFirstSupportedHash()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile iso(isoPath);
    QVERIFY(iso.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(iso.write(data) == data.size());
    iso.close();

    const QString matchingHash =
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex());
    const QString text = QStringLiteral("SHA256 (other.iso) = %1\nSHA512 (sample.iso) = %2\n")
                             .arg(QString(64, QLatin1Char('0')), matchingHash);
    const auto parsed = parseChecksumText(text, isoPath);

    QCOMPARE(parsed.algorithm, QStringLiteral("SHA512"));
    QCOMPARE(parsed.checksum, matchingHash);
    QCOMPARE(parsed.filename, QStringLiteral("sample.iso"));
}

void ChecksumTests::checksumFileWithBomAndCrlfIsParsed()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    const QString checksum = QString(64, QLatin1Char('a'));

    const QString checksumFilePath = tempDir.path() + QStringLiteral("/sample.sha256");
    QFile checksumFile(checksumFilePath);
    QVERIFY(checksumFile.open(QIODevice::WriteOnly));
    QByteArray contents("\xEF\xBB\xBF", 3);
    contents += QStringLiteral("%1  sample.iso\r\n").arg(checksum).toUtf8();
    QVERIFY(checksumFile.write(contents) == contents.size());
    checksumFile.close();

    const auto parsed = loadChecksumFile(checksumFilePath, isoPath);
    QCOMPARE(parsed.algorithm, QStringLiteral("SHA256"));
    QCOMPARE(parsed.checksum, checksum);
    QCOMPARE(parsed.filename, QStringLiteral("sample.iso"));
}

void ChecksumTests::uppercaseChecksumIsNormalizedToLowercase()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    const QString upperChecksum = QString(64, QLatin1Char('A'));

    const auto parsed = parseChecksumText(QStringLiteral("%1  sample.iso").arg(upperChecksum), isoPath);
    QCOMPARE(parsed.checksum, upperChecksum.toLower());
}

void ChecksumTests::commentAndBlankLinesAreSkipped()
{
    QTemporaryDir tempDir;
    const QString isoPath = tempDir.path() + QStringLiteral("/sample.iso");
    const QString checksum = QString(64, QLatin1Char('b'));
    const QString text = QStringLiteral("# generated by sha256sum\n\n; another comment\n%1  sample.iso\n").arg(checksum);

    const auto parsed = parseChecksumText(text, isoPath);
    QCOMPARE(parsed.checksum, checksum);
    QCOMPARE(parsed.lineNumber, 4);
}

void VerifierTests::mismatchIsReported()
{
    QTemporaryDir tempDir;
    const QString filePath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(file.write(data) == data.size());
    file.close();

    const auto result = verifyChecksum(filePath, QString(64, QLatin1Char('0')), QStringLiteral("SHA256"));
    QCOMPARE(result.status, VerificationStatus::Mismatch);
    QVERIFY(result.matches.has_value());
    QVERIFY(!*result.matches);
}

void VerifierTests::missingChecksumGeneratesHash()
{
    QTemporaryDir tempDir;
    const QString filePath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(file.write(data) == data.size());
    file.close();

    const auto result = verifyChecksum(filePath, {}, QStringLiteral("SHA256"));
    QCOMPARE(result.status, VerificationStatus::Generated);
    QVERIFY(!result.matches.has_value());
    QCOMPARE(result.computedHash.size(), 64);
}

void VerifierTests::missingFileIsReported()
{
    QTemporaryDir tempDir;
    const QString missingPath = tempDir.path() + QStringLiteral("/missing.iso");

    const auto result = verifyChecksum(missingPath, QString(64, QLatin1Char('0')), QStringLiteral("SHA256"));
    QCOMPARE(result.status, VerificationStatus::Error);
    QVERIFY(result.message.contains(QStringLiteral("does not exist")));
}

void VerifierTests::parsedChecksumCanVerifyMatch()
{
    QTemporaryDir tempDir;
    const QString filePath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(file.write(data) == data.size());
    file.close();

    const auto parsed = parseChecksumText(
        QStringLiteral("%1  sample.iso")
            .arg(QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())),
        filePath);
    const auto result = verifyChecksum(filePath, parsed.checksum, parsed.algorithm);
    QCOMPARE(result.status, VerificationStatus::Match);
    QVERIFY(result.matches.has_value());
    QVERIFY(*result.matches);
}

void VerifierTests::parsedChecksumCanVerifyMismatch()
{
    QTemporaryDir tempDir;
    const QString filePath = tempDir.path() + QStringLiteral("/sample.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray data = "iso integrity test data";
    QVERIFY(file.write(data) == data.size());
    file.close();

    const auto parsed =
        parseChecksumText(QStringLiteral("%1  sample.iso").arg(QString(64, QLatin1Char('0'))), filePath);
    const auto result = verifyChecksum(filePath, parsed.checksum, parsed.algorithm);
    QCOMPARE(result.status, VerificationStatus::Mismatch);
    QVERIFY(result.matches.has_value());
    QVERIFY(!*result.matches);
}

void VerifierTests::cancellationStopsVerification()
{
    QTemporaryDir tempDir;
    const QString filePath = tempDir.path() + QStringLiteral("/large.bin");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray chunk(1024 * 1024, 'x');
    for (int i = 0; i < 32; ++i) {
        QVERIFY(file.write(chunk) == chunk.size());
    }
    file.close();

    auto cancelToken = makeCancelToken();
    cancelToken->store(true);

    const auto result =
        verifyChecksum(filePath, QString(64, QLatin1Char('0')), QStringLiteral("SHA256"), {}, cancelToken);
    QCOMPARE(result.status, VerificationStatus::Cancelled);
}

void VerifierTests::cancellationMidRunStopsParallelDigests()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.path() + QStringLiteral("/large.bin");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    // Several read-ahead buffers' worth, so the cancel below lands while the
    // per-algorithm worker threads are mid-file rather than before they start.
    QByteArray chunk(1024 * 1024, 'x');
    for (int i = 0; i < 48; ++i) {
        QVERIFY(file.write(chunk) == chunk.size());
    }
    file.close();

    auto cancelToken = makeCancelToken();
    // Requesting four algorithms puts three worker threads behind the caller;
    // cancelling from the progress callback tears them all down mid-chunk.
    const auto result = verifyChecksum(
        filePath,
        QString(64, QLatin1Char('0')),
        QStringLiteral("SHA256"),
        [&cancelToken](qint64) { cancelToken->store(true); },
        cancelToken,
        QStringList{QStringLiteral("SHA512"), QStringLiteral("SHA1"), QStringLiteral("MD5")});

    QCOMPARE(result.status, VerificationStatus::Cancelled);
    QVERIFY(result.computedHashes.isEmpty());
}

void VerifierTests::multiChunkFileHashMatchesReference()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Larger than the whole ring of read-ahead buffers (4 x 8 MB), so slots are
    // recycled at least once, and deliberately not a whole multiple of one, so
    // the digest also covers a partial tail chunk. Byte values vary so a dropped
    // or reordered chunk changes the hash.
    QByteArray data;
    const qsizetype totalSize = (35 * 1024 * 1024) + 12345;
    data.reserve(totalSize);
    for (qsizetype i = 0; i < totalSize; ++i) {
        data.append(static_cast<char>((i * 31 + (i >> 13)) & 0xFF));
    }

    const QString filePath = tempDir.path() + QStringLiteral("/large.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(data) == data.size());
    file.close();

    QCOMPARE(
        calculateFileHash(filePath, QStringLiteral("SHA256")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
    QCOMPARE(
        calculateFileHash(filePath, QStringLiteral("SHA512")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex()));
}

void VerifierTests::unbufferedReadsMatchBufferedDigest()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // The unbuffered path reads in whole sectors, so the interesting case is a
    // file whose size is not a sector multiple: its final read runs past the end
    // of the file and must still hash only the real bytes. The size also crosses
    // the buffer ring so requests are re-issued into recycled slots.
    QByteArray data;
    const qsizetype totalSize = (35 * 1024 * 1024) + 517;
    data.reserve(totalSize);
    for (qsizetype i = 0; i < totalSize; ++i) {
        data.append(static_cast<char>((i * 17 + (i >> 11)) & 0xFF));
    }

    const QString filePath = tempDir.path() + QStringLiteral("/unbuffered.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(data) == data.size());
    file.close();

    if (!bench::unbufferedIoAvailable(filePath)) {
        QSKIP("Unbuffered reads are not available on this platform or filesystem.");
    }

    const QStringList algorithms{QStringLiteral("SHA256"), QStringLiteral("SHA512")};
    const auto unbuffered = bench::hashWithBackend(filePath, algorithms, true, true);

    QCOMPARE(
        unbuffered.value(QStringLiteral("SHA256")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
    QCOMPARE(
        unbuffered.value(QStringLiteral("SHA512")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex()));
}

void VerifierTests::unbufferedReadsHandleSmallAndEmptyFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Fewer bytes than a single buffer, so most of the ring is never issued at
    // all, and then no bytes whatsoever.
    const QByteArray tiny = QByteArrayLiteral("iso-integrity-check");
    const QString tinyPath = tempDir.path() + QStringLiteral("/tiny.iso");
    QFile tinyFile(tinyPath);
    QVERIFY(tinyFile.open(QIODevice::WriteOnly));
    QVERIFY(tinyFile.write(tiny) == tiny.size());
    tinyFile.close();

    const QString emptyPath = tempDir.path() + QStringLiteral("/empty.iso");
    QFile emptyFile(emptyPath);
    QVERIFY(emptyFile.open(QIODevice::WriteOnly));
    emptyFile.close();

    if (!bench::unbufferedIoAvailable(tinyPath)) {
        QSKIP("Unbuffered reads are not available on this platform or filesystem.");
    }

    QCOMPARE(
        bench::hashWithBackend(tinyPath, {QStringLiteral("SHA256")}, true, true).value(QStringLiteral("SHA256")),
        QString::fromLatin1(QCryptographicHash::hash(tiny, QCryptographicHash::Sha256).toHex()));
    QCOMPARE(
        bench::hashWithBackend(emptyPath, {QStringLiteral("SHA256")}, true, true).value(QStringLiteral("SHA256")),
        QString::fromLatin1(QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256).toHex()));
}

void VerifierTests::emptyFileHashesToTheEmptyDigest()
{
    QTemporaryDir tempDir;
    const QString filePath = tempDir.path() + QStringLiteral("/empty.iso");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    QCOMPARE(
        calculateFileHash(filePath, QStringLiteral("SHA256")),
        QString::fromLatin1(QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256).toHex()));
}

void VerifierTests::directoryPathIsReportedAsError()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto result = verifyChecksum(tempDir.path(), QString(64, QLatin1Char('0')), QStringLiteral("SHA256"));
    QCOMPARE(result.status, VerificationStatus::Error);
    QVERIFY(!result.matches.has_value());
}

namespace {

QString writeSampleFile(const QTemporaryDir& tempDir, const QByteArray& data, const QString& name)
{
    const QString filePath = tempDir.path() + QLatin1Char('/') + name;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        return {};
    }
    file.close();
    return filePath;
}

QByteArray varyingBytes(qsizetype size)
{
    QByteArray data;
    data.reserve(size);
    for (qsizetype i = 0; i < size; ++i) {
        data.append(static_cast<char>((i * 31 + (i >> 13)) & 0xFF));
    }
    return data;
}

} // namespace

void VerifierTests::singlePassComputesEveryRequestedAlgorithm()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray data = "iso integrity test data";
    const QString filePath = writeSampleFile(tempDir, data, QStringLiteral("sample.iso"));
    QVERIFY(!filePath.isEmpty());

    const auto hashes = calculateFileHashes(
        filePath,
        {QStringLiteral("SHA256"), QStringLiteral("SHA512"), QStringLiteral("SHA1"), QStringLiteral("MD5")});

    QCOMPARE(hashes.size(), 4);
    QCOMPARE(
        hashes.value(QStringLiteral("SHA256")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
    QCOMPARE(
        hashes.value(QStringLiteral("SHA512")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex()));
    QCOMPARE(
        hashes.value(QStringLiteral("SHA1")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex()));
    QCOMPARE(
        hashes.value(QStringLiteral("MD5")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex()));
}

void VerifierTests::singlePassAgreesWithSequentialHashingAcrossChunks()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Spans several internal 8 MB read-ahead buffers plus a partial tail, so a
    // digest that was fed chunks out of order or skipped one would diverge.
    const QByteArray data = varyingBytes((17 * 1024 * 1024) + 12345);
    const QString filePath = writeSampleFile(tempDir, data, QStringLiteral("large.iso"));
    QVERIFY(!filePath.isEmpty());

    const auto combined =
        calculateFileHashes(filePath, {QStringLiteral("SHA256"), QStringLiteral("SHA512"), QStringLiteral("MD5")});

    QCOMPARE(combined.value(QStringLiteral("SHA256")), calculateFileHash(filePath, QStringLiteral("SHA256")));
    QCOMPARE(combined.value(QStringLiteral("SHA512")), calculateFileHash(filePath, QStringLiteral("SHA512")));
    QCOMPARE(combined.value(QStringLiteral("MD5")), calculateFileHash(filePath, QStringLiteral("MD5")));
}

void VerifierTests::duplicateAlgorithmsAreCollapsed()
{
    QTemporaryDir tempDir;
    const QByteArray data = "iso integrity test data";
    const QString filePath = writeSampleFile(tempDir, data, QStringLiteral("sample.iso"));
    QVERIFY(!filePath.isEmpty());

    const auto hashes = calculateFileHashes(
        filePath, {QStringLiteral("SHA256"), QStringLiteral("SHA256"), QStringLiteral("SHA1")});

    QCOMPARE(hashes.size(), 2);
    QCOMPARE(
        hashes.value(QStringLiteral("SHA256")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
}

void VerifierTests::unsupportedAlgorithmInListIsRejected()
{
    QTemporaryDir tempDir;
    const QString filePath = writeSampleFile(tempDir, QByteArray("data"), QStringLiteral("sample.iso"));
    QVERIFY(!filePath.isEmpty());

    bool threw = false;
    try {
        calculateFileHashes(filePath, {QStringLiteral("SHA256"), QStringLiteral("CRC32")});
    } catch (const std::exception& error) {
        threw = true;
        QVERIFY(QString::fromUtf8(error.what()).contains(QStringLiteral("CRC32")));
    }
    QVERIFY(threw);
}

void VerifierTests::emptyAlgorithmListIsRejected()
{
    QTemporaryDir tempDir;
    const QString filePath = writeSampleFile(tempDir, QByteArray("data"), QStringLiteral("sample.iso"));
    QVERIFY(!filePath.isEmpty());

    bool threw = false;
    try {
        calculateFileHashes(filePath, {});
    } catch (const std::exception&) {
        threw = true;
    }
    QVERIFY(threw);
}

void VerifierTests::alsoComputeReturnsExtraHashesWithoutAffectingTheVerdict()
{
    QTemporaryDir tempDir;
    const QByteArray data = "iso integrity test data";
    const QString filePath = writeSampleFile(tempDir, data, QStringLiteral("sample.iso"));
    QVERIFY(!filePath.isEmpty());

    const QString expected = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    const auto result = verifyChecksum(
        filePath,
        expected,
        QStringLiteral("SHA256"),
        {},
        {},
        {QStringLiteral("SHA512"), QStringLiteral("MD5"), QStringLiteral("NOT_A_HASH")});

    QCOMPARE(result.status, VerificationStatus::Match);
    QCOMPARE(result.computedHash, expected);
    // The unsupported name is ignored rather than failing the whole run.
    QCOMPARE(result.computedHashes.size(), 3);
    QCOMPARE(
        result.computedHashes.value(QStringLiteral("SHA512")),
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex()));
    QVERIFY(!result.computedHashes.contains(QStringLiteral("NOT_A_HASH")));
}

void VerifierTests::singlePassReportsProgressOncePerChunk()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray data = varyingBytes((17 * 1024 * 1024) + 999);
    const QString filePath = writeSampleFile(tempDir, data, QStringLiteral("large.iso"));
    QVERIFY(!filePath.isEmpty());

    // Progress must track bytes read, not bytes fed to digests — otherwise three
    // algorithms would report three times the file size.
    QList<qint64> reported;
    calculateFileHashes(
        filePath,
        {QStringLiteral("SHA256"), QStringLiteral("SHA512"), QStringLiteral("MD5")},
        [&reported](qint64 bytesRead) { reported.append(bytesRead); });

    QVERIFY(!reported.isEmpty());
    QCOMPARE(reported.last(), static_cast<qint64>(data.size()));
    for (qsizetype i = 1; i < reported.size(); ++i) {
        QVERIFY(reported.at(i) > reported.at(i - 1));
    }
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    int status = 0;
    ChecksumTests checksumTests;
    status |= QTest::qExec(&checksumTests, argc, argv);

    VerifierTests verifierTests;
    status |= QTest::qExec(&verifierTests, argc, argv);

    return status;
}

#include "test_core.moc"
