#include "core/verifier.h"

#include "core/checksum.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>

#include <stdexcept>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#ifdef ISO_HAS_OPENSSL
#include <openssl/evp.h>
#endif

namespace iso {
namespace {

constexpr qint64 HashBufferSize = 8 * 1024 * 1024;

struct HashCancelledException : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void throwIfCancelled(const CancelToken& cancelToken)
{
    if (cancelToken && cancelToken->load()) {
        throw HashCancelledException("Verification was cancelled.");
    }
}

void throwOnReadError(const QFile& file)
{
    if (file.error() != QFileDevice::NoError) {
        throw std::runtime_error(
            QStringLiteral("The selected file could not be read: %1").arg(file.errorString()).toStdString());
    }
}

// Guards against a silently truncated read. QFile::read() returns an empty
// buffer both at EOF and when a driver hands back zero bytes without setting an
// error (removable media pulled mid-read, a dropped network share). Without this
// check the digest of a partial file would be reported as a legitimate result.
void throwOnIncompleteRead(qint64 bytesHashed, qint64 expectedSize)
{
    if (expectedSize <= 0 || bytesHashed == expectedSize) {
        return;
    }

    const QString detail = bytesHashed < expectedSize
                               ? QStringLiteral("only %1 of %2 bytes could be read").arg(bytesHashed).arg(expectedSize)
                               : QStringLiteral("%1 bytes were read but the file reported %2")
                                     .arg(bytesHashed)
                                     .arg(expectedSize);
    throw std::runtime_error(QStringLiteral("The file was not read completely (%1). It may have been modified, or the "
                                            "drive disconnected, during verification.")
                                 .arg(detail)
                                 .toStdString());
}

template <typename HashChunkFn>
void hashFileWithReadAhead(QFile& file, HashChunkFn&& hashChunk, const CancelToken& cancelToken)
{
    const qint64 expectedSize = file.size();
    qint64 totalBytesHashed = 0;

    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable consumed;
    QByteArray pendingBuffer;
    std::exception_ptr readError;
    bool hasPendingBuffer = false;
    bool readFinished = false;
    bool stopReader = false;

    std::thread reader([&]() {
        try {
            while (true) {
                QByteArray buffer = file.read(HashBufferSize);
                if (buffer.isEmpty()) {
                    throwOnReadError(file);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        readFinished = true;
                    }
                    ready.notify_one();
                    return;
                }

                std::unique_lock<std::mutex> lock(mutex);
                consumed.wait(lock, [&]() { return !hasPendingBuffer || stopReader; });
                if (stopReader) {
                    return;
                }
                pendingBuffer = std::move(buffer);
                hasPendingBuffer = true;
                lock.unlock();
                ready.notify_one();
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            readError = std::current_exception();
            readFinished = true;
            ready.notify_one();
        }
    });

    struct ReaderJoinGuard {
        std::thread& reader;
        std::mutex& mutex;
        std::condition_variable& consumed;
        bool& stopReader;
        ~ReaderJoinGuard()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopReader = true;
            }
            consumed.notify_one();
            if (reader.joinable()) {
                reader.join();
            }
        }
    } readerJoinGuard{reader, mutex, consumed, stopReader};

    while (true) {
        QByteArray buffer;
        {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [&]() { return hasPendingBuffer || readFinished || readError; });
            if (readError) {
                std::rethrow_exception(readError);
            }
            if (!hasPendingBuffer && readFinished) {
                break;
            }

            buffer = std::move(pendingBuffer);
            hasPendingBuffer = false;
        }
        consumed.notify_one();

        throwIfCancelled(cancelToken);
        totalBytesHashed += buffer.size();
        hashChunk(buffer);
    }

    throwIfCancelled(cancelToken);
    throwOnIncompleteRead(totalBytesHashed, expectedSize);
}

#ifdef _WIN32

struct CngError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

LPCWSTR cngAlgorithmId(const QString& algorithm)
{
    if (algorithm == QStringLiteral("SHA256")) {
        return BCRYPT_SHA256_ALGORITHM;
    }
    if (algorithm == QStringLiteral("SHA512")) {
        return BCRYPT_SHA512_ALGORITHM;
    }
    if (algorithm == QStringLiteral("SHA1")) {
        return BCRYPT_SHA1_ALGORITHM;
    }
    if (algorithm == QStringLiteral("MD5")) {
        return BCRYPT_MD5_ALGORITHM;
    }
    return nullptr;
}

class CngHasher {
  public:
    explicit CngHasher(LPCWSTR algorithmId)
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, algorithmId, nullptr, 0) < 0) {
            throw CngError("Failed to open the system hash provider.");
        }
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw CngError("Failed to create the hash object.");
        }
    }

    ~CngHasher()
    {
        if (hash_) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    CngHasher(const CngHasher&) = delete;
    CngHasher& operator=(const CngHasher&) = delete;

    void update(const char* data, qsizetype length)
    {
        if (length <= 0) {
            return;
        }
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(length), 0) <
            0) {
            throw CngError("Failed while hashing file data.");
        }
    }

    QByteArray finish()
    {
        DWORD hashLength = 0;
        DWORD written = 0;
        if (BCryptGetProperty(
                hash_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &written, 0) <
            0) {
            throw CngError("Failed to query the hash length.");
        }

        QByteArray result(static_cast<qsizetype>(hashLength), '\0');
        if (BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(result.data()), hashLength, 0) < 0) {
            throw CngError("Failed to finalize the hash.");
        }
        return result;
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

QString
hashWithCng(QFile& file, LPCWSTR algorithmId, const ProgressCallback& progressCallback, const CancelToken& cancelToken)
{
    CngHasher hasher(algorithmId);
    qint64 bytesRead = 0;

    hashFileWithReadAhead(
        file,
        [&](const QByteArray& chunk) {
            hasher.update(chunk.constData(), chunk.size());
            bytesRead += chunk.size();
            if (progressCallback) {
                progressCallback(bytesRead);
            }
        },
        cancelToken);

    return QString::fromLatin1(hasher.finish().toHex());
}

#endif // _WIN32

#ifdef ISO_HAS_OPENSSL

struct EvpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

const EVP_MD* evpAlgorithm(const QString& algorithm)
{
    if (algorithm == QStringLiteral("SHA256")) {
        return EVP_sha256();
    }
    if (algorithm == QStringLiteral("SHA512")) {
        return EVP_sha512();
    }
    if (algorithm == QStringLiteral("SHA1")) {
        return EVP_sha1();
    }
    if (algorithm == QStringLiteral("MD5")) {
        return EVP_md5();
    }
    return nullptr;
}

class EvpHasher {
  public:
    explicit EvpHasher(const EVP_MD* algorithm) : context_(EVP_MD_CTX_new())
    {
        if (!context_) {
            throw EvpError("Failed to create the OpenSSL hash context.");
        }
        if (EVP_DigestInit_ex(context_, algorithm, nullptr) != 1) {
            throw EvpError("Failed to initialize the OpenSSL hash.");
        }
    }

    ~EvpHasher() { EVP_MD_CTX_free(context_); }

    EvpHasher(const EvpHasher&) = delete;
    EvpHasher& operator=(const EvpHasher&) = delete;

    void update(const char* data, qsizetype length)
    {
        if (length <= 0) {
            return;
        }
        if (EVP_DigestUpdate(context_, data, static_cast<size_t>(length)) != 1) {
            throw EvpError("Failed while hashing file data with OpenSSL.");
        }
    }

    QByteArray finish()
    {
        QByteArray result(EVP_MAX_MD_SIZE, '\0');
        unsigned int hashLength = 0;
        if (EVP_DigestFinal_ex(context_, reinterpret_cast<unsigned char*>(result.data()), &hashLength) != 1) {
            throw EvpError("Failed to finalize the OpenSSL hash.");
        }
        result.resize(static_cast<qsizetype>(hashLength));
        return result;
    }

  private:
    EVP_MD_CTX* context_ = nullptr;
};

QString hashWithOpenSsl(
    QFile& file, const EVP_MD* algorithm, const ProgressCallback& progressCallback, const CancelToken& cancelToken)
{
    EvpHasher hasher(algorithm);
    qint64 bytesRead = 0;

    hashFileWithReadAhead(
        file,
        [&](const QByteArray& chunk) {
            hasher.update(chunk.constData(), chunk.size());
            bytesRead += chunk.size();
            if (progressCallback) {
                progressCallback(bytesRead);
            }
        },
        cancelToken);

    return QString::fromLatin1(hasher.finish().toHex());
}

#endif // ISO_HAS_OPENSSL

QString hashWithQt(
    QFile& file,
    QCryptographicHash::Algorithm algorithm,
    const ProgressCallback& progressCallback,
    const CancelToken& cancelToken)
{
    QCryptographicHash digest(algorithm);
    qint64 bytesRead = 0;

    hashFileWithReadAhead(
        file,
        [&](const QByteArray& chunk) {
            digest.addData(chunk);
            bytesRead += chunk.size();
            if (progressCallback) {
                progressCallback(bytesRead);
            }
        },
        cancelToken);

    return QString::fromLatin1(digest.result().toHex());
}

} // namespace

QString calculateFileHash(
    const QString& filePath, const QString& algorithm, ProgressCallback progressCallback, CancelToken cancelToken)
{
    const auto hashes = supportedHashes();
    if (!hashes.contains(algorithm)) {
        throw std::runtime_error(QStringLiteral("Unsupported hash algorithm: %1").arg(algorithm).toStdString());
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(
            QStringLiteral("The selected file could not be opened: %1").arg(file.errorString()).toStdString());
    }

    // A backend failure restarts the read from byte 0, so the listener must be
    // told progress rewound — otherwise it keeps comparing against the byte
    // count from the abandoned attempt and its throughput estimate stalls.
    const auto restartAfterBackendFailure = [&]() {
        file.seek(0);
        if (progressCallback) {
            progressCallback(0);
        }
    };
    Q_UNUSED(restartAfterBackendFailure);

#ifdef _WIN32
    if (LPCWSTR algorithmId = cngAlgorithmId(algorithm)) {
        try {
            return hashWithCng(file, algorithmId, progressCallback, cancelToken);
        } catch (const CngError&) {
            restartAfterBackendFailure();
        }
    }
#endif

#ifdef ISO_HAS_OPENSSL
    if (const EVP_MD* algorithmId = evpAlgorithm(algorithm)) {
        try {
            return hashWithOpenSsl(file, algorithmId, progressCallback, cancelToken);
        } catch (const EvpError&) {
            restartAfterBackendFailure();
        }
    }
#endif

    return hashWithQt(file, hashes.value(algorithm).qtAlgorithm, progressCallback, cancelToken);
}

VerificationResult verifyChecksum(
    const QString& filePath,
    const QString& expectedChecksum,
    const QString& algorithm,
    ProgressCallback progressCallback,
    CancelToken cancelToken)
{
    const QFileInfo info(filePath);
    if (filePath.isEmpty()) {
        return {VerificationStatus::Error, QStringLiteral("Choose an ISO file first."), {}, std::nullopt};
    }

    if (!supportedHashes().contains(algorithm)) {
        return {
            VerificationStatus::Error,
            QStringLiteral("Unsupported hash algorithm: %1").arg(algorithm),
            {},
            std::nullopt};
    }

    if (!info.exists()) {
        return {VerificationStatus::Error, QStringLiteral("The selected file does not exist."), {}, std::nullopt};
    }

    if (!info.isFile()) {
        return {VerificationStatus::Error, QStringLiteral("The selected path is not a file."), {}, std::nullopt};
    }

    const auto validationError = validateExpectedChecksum(expectedChecksum, algorithm);
    if (validationError.has_value()) {
        return {VerificationStatus::Error, *validationError, {}, std::nullopt};
    }

    const QString normalizedExpected = normalizeChecksum(expectedChecksum);

    QString computedHash;
    try {
        computedHash = calculateFileHash(filePath, algorithm, std::move(progressCallback), cancelToken);
    } catch (const HashCancelledException&) {
        return {
            VerificationStatus::Cancelled,
            QStringLiteral("Verification cancelled."),
            {},
            std::nullopt,
        };
    } catch (const std::exception& error) {
        return {
            VerificationStatus::Error,
            QString::fromUtf8(error.what()),
            {},
            std::nullopt,
        };
    }

    if (normalizedExpected.isEmpty()) {
        return {
            VerificationStatus::Generated,
            QStringLiteral("Checksum calculated. Paste or import an official checksum to verify integrity."),
            computedHash,
            std::nullopt,
        };
    }

    if (computedHash == normalizedExpected) {
        return {
            VerificationStatus::Match,
            QStringLiteral("The ISO checksum matches the expected value."),
            computedHash,
            true,
        };
    }

    return {
        VerificationStatus::Mismatch,
        QStringLiteral("The ISO checksum does not match the expected value."),
        computedHash,
        false,
    };
}

QString formatStatusMessage(VerificationStatus status, const QString& message)
{
    switch (status) {
    case VerificationStatus::Match:
        return QStringLiteral("Match: %1").arg(message);
    case VerificationStatus::Mismatch:
        return QStringLiteral("Mismatch: %1").arg(message);
    case VerificationStatus::Error:
        return QStringLiteral("Error: %1").arg(message);
    case VerificationStatus::Generated:
        return message;
    case VerificationStatus::Cancelled:
        return QStringLiteral("Cancelled: %1").arg(message);
    }
    return message;
}

} // namespace iso
