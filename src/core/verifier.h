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

// How reading the file interacts with the operating system's page cache.
//
// Buffered is the default and is the faster choice for the case this app sees
// most: checking an ISO that was just downloaded, and so is still largely
// resident in the cache. Unbuffered gives that up in exchange for not evicting
// the rest of the system's working set to hold several gigabytes of ISO that
// will never be read a second time. It also avoids a copy by way of the cache,
// which makes it the quicker option once the file is genuinely cold.
enum class IoPolicy {
    Buffered,
    Unbuffered,
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
    CancelToken cancelToken = {},
    IoPolicy ioPolicy = IoPolicy::Buffered);

// Computes every requested algorithm from a single read pass, so asking for
// SHA256 and SHA512 costs one read of the file rather than two. Order is
// preserved for the read; duplicates are dropped. Throws on the same conditions
// as calculateFileHash.
QHash<QString, QString> calculateFileHashes(
    const QString& filePath,
    const QStringList& algorithms,
    ProgressCallback progressCallback = {},
    CancelToken cancelToken = {},
    IoPolicy ioPolicy = IoPolicy::Buffered);

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
    const QStringList& alsoCompute = {},
    IoPolicy ioPolicy = IoPolicy::Buffered);

QString formatStatusMessage(VerificationStatus status, const QString& message);

// Instrumentation for the iso-hash-bench tool, not part of the app's API. These
// expose the read pipeline and the backend choice so a benchmark can tell an
// I/O-bound run from a compute-bound one — the two call for opposite
// optimisations, and guessing between them is how tuning effort gets wasted.
// Both throw on the same conditions as calculateFileHashes().
namespace bench {

// Runs the same read pipeline as hashing but feeds the bytes to no digest, so
// the elapsed time is this file's read ceiling. Returns bytes read.
qint64 readOnlyPass(const QString& filePath, bool useUnbufferedIo = false, CancelToken cancelToken = {});

// calculateFileHashes() with the backend and the I/O mode pinned instead of
// chosen for you, so the combinations can be timed against each other.
QHash<QString, QString> hashWithBackend(
    const QString& filePath,
    const QStringList& algorithms,
    bool useNativeBackends,
    bool useUnbufferedIo = false,
    CancelToken cancelToken = {});

// Whether this file can actually be opened for unbuffered reads. Requesting
// unbuffered I/O for a file that cannot support it silently falls back to
// buffered reads, which would otherwise make a benchmark report the same number
// twice under two different labels.
bool unbufferedIoAvailable(const QString& filePath);

} // namespace bench

} // namespace iso
