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

// One digest being fed the file's bytes. Wrapping each backend behind this lets
// a single read pass drive several algorithms at once, so computing SHA256 and
// SHA512 for an ISO costs one read instead of two.
class Hasher {
  public:
    virtual ~Hasher() = default;
    virtual void update(const char* data, qsizetype length) = 0;
    virtual QByteArray finish() = 0;
};

// Thrown when a native backend fails. The caller retries the whole pass with Qt
// hashing rather than leaving some digests native and others not.
struct NativeBackendError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#ifdef _WIN32

struct CngError : NativeBackendError {
    using NativeBackendError::NativeBackendError;
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

class CngHasher final : public Hasher {
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

    ~CngHasher() override
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

    void update(const char* data, qsizetype length) override
    {
        if (length <= 0) {
            return;
        }
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(length), 0) <
            0) {
            throw CngError("Failed while hashing file data.");
        }
    }

    QByteArray finish() override
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

#endif // _WIN32

#ifdef ISO_HAS_OPENSSL

struct EvpError : NativeBackendError {
    using NativeBackendError::NativeBackendError;
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

class EvpHasher final : public Hasher {
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

    ~EvpHasher() override { EVP_MD_CTX_free(context_); }

    EvpHasher(const EvpHasher&) = delete;
    EvpHasher& operator=(const EvpHasher&) = delete;

    void update(const char* data, qsizetype length) override
    {
        if (length <= 0) {
            return;
        }
        if (EVP_DigestUpdate(context_, data, static_cast<size_t>(length)) != 1) {
            throw EvpError("Failed while hashing file data with OpenSSL.");
        }
    }

    QByteArray finish() override
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

#endif // ISO_HAS_OPENSSL

class QtHasher final : public Hasher {
  public:
    explicit QtHasher(QCryptographicHash::Algorithm algorithm) : digest_(algorithm) {}

    void update(const char* data, qsizetype length) override
    {
        if (length <= 0) {
            return;
        }
        digest_.addData(QByteArrayView(data, length));
    }

    QByteArray finish() override { return digest_.result(); }

  private:
    QCryptographicHash digest_;
};

// Returns nullptr when no native backend covers this algorithm, in which case
// the caller falls back to Qt. Construction failures surface as
// NativeBackendError so the whole pass restarts on Qt hashing.
std::unique_ptr<Hasher> makeNativeHasher(const QString& algorithm)
{
#ifdef _WIN32
    if (LPCWSTR algorithmId = cngAlgorithmId(algorithm)) {
        return std::make_unique<CngHasher>(algorithmId);
    }
#endif
#ifdef ISO_HAS_OPENSSL
    if (const EVP_MD* algorithmId = evpAlgorithm(algorithm)) {
        return std::make_unique<EvpHasher>(algorithmId);
    }
#endif
    Q_UNUSED(algorithm);
    return nullptr;
}

struct NamedHasher {
    QString algorithm;
    std::unique_ptr<Hasher> hasher;
};

// Reads the file once and feeds every chunk to all requested digests.
QHash<QString, QString> hashInOnePass(
    QFile& file,
    const QStringList& algorithms,
    bool useNativeBackends,
    const ProgressCallback& progressCallback,
    const CancelToken& cancelToken)
{
    const auto& hashes = supportedHashes();
    std::vector<NamedHasher> hashers;
    hashers.reserve(static_cast<size_t>(algorithms.size()));

    for (const QString& algorithm : algorithms) {
        std::unique_ptr<Hasher> hasher;
        if (useNativeBackends) {
            hasher = makeNativeHasher(algorithm);
        }
        if (!hasher) {
            hasher = std::make_unique<QtHasher>(hashes.value(algorithm).qtAlgorithm);
        }
        hashers.push_back(NamedHasher{algorithm, std::move(hasher)});
    }

    qint64 bytesRead = 0;
    hashFileWithReadAhead(
        file,
        [&](const QByteArray& chunk) {
            for (NamedHasher& entry : hashers) {
                entry.hasher->update(chunk.constData(), chunk.size());
            }
            bytesRead += chunk.size();
            if (progressCallback) {
                progressCallback(bytesRead);
            }
        },
        cancelToken);

    QHash<QString, QString> results;
    results.reserve(static_cast<qsizetype>(hashers.size()));
    for (NamedHasher& entry : hashers) {
        results.insert(entry.algorithm, QString::fromLatin1(entry.hasher->finish().toHex()));
    }
    return results;
}

// Drops duplicates while preserving the caller's order, and rejects unknown names.
QStringList normalizeAlgorithmList(const QStringList& algorithms)
{
    QStringList unique;
    for (const QString& algorithm : algorithms) {
        if (!supportedHashes().contains(algorithm)) {
            throw std::runtime_error(QStringLiteral("Unsupported hash algorithm: %1").arg(algorithm).toStdString());
        }
        if (!unique.contains(algorithm)) {
            unique.append(algorithm);
        }
    }
    if (unique.isEmpty()) {
        throw std::runtime_error("No hash algorithm was requested.");
    }
    return unique;
}

} // namespace

QHash<QString, QString> calculateFileHashes(
    const QString& filePath,
    const QStringList& algorithms,
    ProgressCallback progressCallback,
    CancelToken cancelToken)
{
    const QStringList requested = normalizeAlgorithmList(algorithms);

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(
            QStringLiteral("The selected file could not be opened: %1").arg(file.errorString()).toStdString());
    }

    try {
        return hashInOnePass(file, requested, true, progressCallback, cancelToken);
    } catch (const NativeBackendError&) {
        // Restart the whole pass on Qt hashing. The listener is told progress
        // rewound, otherwise it keeps comparing against the byte count from the
        // abandoned attempt and its throughput estimate stalls.
        file.seek(0);
        if (progressCallback) {
            progressCallback(0);
        }
        return hashInOnePass(file, requested, false, progressCallback, cancelToken);
    }
}

QString calculateFileHash(
    const QString& filePath, const QString& algorithm, ProgressCallback progressCallback, CancelToken cancelToken)
{
    return calculateFileHashes(filePath, {algorithm}, std::move(progressCallback), std::move(cancelToken))
        .value(algorithm);
}

VerificationResult verifyChecksum(
    const QString& filePath,
    const QString& expectedChecksum,
    const QString& algorithm,
    ProgressCallback progressCallback,
    CancelToken cancelToken,
    const QStringList& alsoCompute)
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
    QHash<QString, QString> computedHashes;
    try {
        QStringList requested{algorithm};
        for (const QString& extra : alsoCompute) {
            if (supportedHashes().contains(extra) && !requested.contains(extra)) {
                requested.append(extra);
            }
        }
        computedHashes = calculateFileHashes(filePath, requested, std::move(progressCallback), cancelToken);
        computedHash = computedHashes.value(algorithm);
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
            computedHashes,
        };
    }

    if (computedHash == normalizedExpected) {
        return {
            VerificationStatus::Match,
            QStringLiteral("The ISO checksum matches the expected value."),
            computedHash,
            true,
            computedHashes,
        };
    }

    return {
        VerificationStatus::Mismatch,
        QStringLiteral("The ISO checksum does not match the expected value."),
        computedHash,
        false,
        computedHashes,
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
