#pragma once

#include <QString>
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
};

using ProgressCallback = std::function<void(qint64 bytesRead)>;
using CancelToken = std::shared_ptr<std::atomic<bool>>;

inline CancelToken makeCancelToken()
{
    return std::make_shared<std::atomic<bool>>(false);
}

QString calculateFileHash(
    const QString& filePath,
    const QString& algorithm,
    ProgressCallback progressCallback = {},
    CancelToken cancelToken = {});

VerificationResult verifyChecksum(
    const QString& filePath,
    const QString& expectedChecksum,
    const QString& algorithm,
    ProgressCallback progressCallback = {},
    CancelToken cancelToken = {});

QString formatStatusMessage(VerificationStatus status, const QString& message);

} // namespace iso
