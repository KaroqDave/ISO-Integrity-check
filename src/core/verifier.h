#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace iso {

enum class VerificationStatus {
    Match,
    Mismatch,
    Error,
    Generated,
    Cancelled,
};

struct VerificationResult {
    VerificationStatus status = VerificationStatus::Error;
    QString message;
    QString computedHash;
    std::optional<bool> matches;
    // Every algorithm computed during the run, keyed by name. Contains at least
    // the verified algorithm; more when extras were requested via alsoCompute.
    QHash<QString, QString> computedHashes;
};

using ProgressCallback = std::function<void(qint64 bytesRead)>;
using CancelToken = std::shared_ptr<std::atomic<bool>>;

inline CancelToken makeCancelToken()
{
    return std::make_shared<std::atomic<bool>>(false);
}

// Throws std::runtime_error when the file cannot be opened or is not read to
// completion. Prefer verifyChecksum(), which reports those as an Error result.
QString calculateFileHash(
    const QString& filePath,
    const QString& algorithm,
    ProgressCallback progressCallback = {},
    CancelToken cancelToken = {});

// Computes every requested algorithm from a single read pass, so asking for
// SHA256 and SHA512 costs one read of the file rather than two. Order is
// preserved for the read; duplicates are dropped. Throws on the same conditions
// as calculateFileHash.
QHash<QString, QString> calculateFileHashes(
    const QString& filePath,
    const QStringList& algorithms,
    ProgressCallback progressCallback = {},
    CancelToken cancelToken = {});

// Never throws: I/O and hashing failures come back as VerificationStatus::Error.
// Algorithms named in alsoCompute are computed in the same pass and returned in
// VerificationResult::computedHashes; only `algorithm` is compared against the
// expected value. Unsupported names in alsoCompute are ignored.
VerificationResult verifyChecksum(
    const QString& filePath,
    const QString& expectedChecksum,
    const QString& algorithm,
    ProgressCallback progressCallback = {},
    CancelToken cancelToken = {},
    const QStringList& alsoCompute = {});

QString formatStatusMessage(VerificationStatus status, const QString& message);

} // namespace iso
