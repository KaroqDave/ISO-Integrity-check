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

    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()).toUpper();

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

    const QString matchingHash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    const QString text = QStringLiteral("%1  other.iso\n%2  sample.iso\n").arg(QString(64, QLatin1Char('0')), matchingHash);
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

    const QString matchingHash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    const QString text = QStringLiteral("%1  sample.iso.zsync\n%2  sample.iso\n")
        .arg(QString(64, QLatin1Char('0')), matchingHash);
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

    const QString matchingHash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex());
    const QString text = QStringLiteral("SHA256 (other.iso) = %1\nSHA512 (sample.iso) = %2\n")
        .arg(QString(64, QLatin1Char('0')), matchingHash);
    const auto parsed = parseChecksumText(text, isoPath);

    QCOMPARE(parsed.algorithm, QStringLiteral("SHA512"));
    QCOMPARE(parsed.checksum, matchingHash);
    QCOMPARE(parsed.filename, QStringLiteral("sample.iso"));
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
        QStringLiteral("%1  sample.iso").arg(QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())),
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

    const auto parsed = parseChecksumText(QStringLiteral("%1  sample.iso").arg(QString(64, QLatin1Char('0'))), filePath);
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

    const auto result = verifyChecksum(filePath, QString(64, QLatin1Char('0')), QStringLiteral("SHA256"), {}, cancelToken);
    QCOMPARE(result.status, VerificationStatus::Cancelled);
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
