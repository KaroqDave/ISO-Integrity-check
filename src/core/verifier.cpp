#include "core/verifier.h"

#include "core/checksum.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>

#include <stdexcept>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <bcrypt.h>
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
        throw std::runtime_error(QStringLiteral("The selected file could not be read: %1").arg(file.errorString()).toStdString());
    }
}

template<typename HashChunkFn>
void hashFileWithReadAhead(QFile& file, HashChunkFn&& hashChunk, const ProgressCallback& progressCallback, const CancelToken& cancelToken)
{
    QByteArray buffer = file.read(HashBufferSize);
    if (buffer.isEmpty()) {
        throwOnReadError(file);
        return;
    }

    QByteArray nextBuffer;
    std::thread reader;

    while (!buffer.isEmpty()) {
        throwIfCancelled(cancelToken);

        reader = std::thread([&file, &nextBuffer]() {
            nextBuffer = file.read(HashBufferSize);
        });

        hashChunk(buffer);
        reader.join();

        if (nextBuffer.isEmpty()) {
            throwOnReadError(file);
            break;
        }

        buffer = std::move(nextBuffer);
    }

    throwIfCancelled(cancelToken);
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
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(length), 0) < 0) {
            throw CngError("Failed while hashing file data.");
        }
    }

    QByteArray finish()
    {
        DWORD hashLength = 0;
        DWORD written = 0;
        if (BCryptGetProperty(hash_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &written, 0) < 0) {
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

QString hashWithCng(QFile& file, LPCWSTR algorithmId, const ProgressCallback& progressCallback, const CancelToken& cancelToken)
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
        progressCallback,
        cancelToken);

    return QString::fromLatin1(hasher.finish().toHex());
}

#endif // _WIN32

QString hashWithQt(QFile& file, QCryptographicHash::Algorithm algorithm, const ProgressCallback& progressCallback, const CancelToken& cancelToken)
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
        progressCallback,
        cancelToken);

    return QString::fromLatin1(digest.result().toHex());
}

} // namespace

QString calculateFileHash(const QString& filePath, const QString& algorithm, ProgressCallback progressCallback, CancelToken cancelToken)
{
    const auto hashes = supportedHashes();
    if (!hashes.contains(algorithm)) {
        throw std::runtime_error(QStringLiteral("Unsupported hash algorithm: %1").arg(algorithm).toStdString());
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(QStringLiteral("The selected file could not be opened: %1").arg(file.errorString()).toStdString());
    }

#ifdef _WIN32
    if (LPCWSTR algorithmId = cngAlgorithmId(algorithm)) {
        try {
            return hashWithCng(file, algorithmId, progressCallback, cancelToken);
        } catch (const CngError&) {
            file.seek(0);
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
        return {VerificationStatus::Error, QStringLiteral("Unsupported hash algorithm: %1").arg(algorithm), {}, std::nullopt};
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
