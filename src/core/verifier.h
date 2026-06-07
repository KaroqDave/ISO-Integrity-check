#pragma once

#include <QString>
#include <QtGlobal>

#include <functional>
#include <optional>

namespace iso {

enum class VerificationStatus {
    Match,
    Mismatch,
    Error,
    Generated,
};

struct VerificationResult {
    VerificationStatus status = VerificationStatus::Error;
    QString message;
    QString computedHash;
    std::optional<bool> matches;
};

using ProgressCallback = std::function<void(qint64 bytesRead)>;

QString calculateFileHash(const QString& filePath, const QString& algorithm, ProgressCallback progressCallback = {});
VerificationResult verifyChecksum(const QString& filePath, const QString& expectedChecksum, const QString& algorithm);

} // namespace iso
