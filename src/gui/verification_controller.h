#pragma once

#include "core/verifier.h"

#include <QObject>
#include <QString>

class QThread;

class VerificationController : public QObject {
    Q_OBJECT

  public:
    explicit VerificationController(QObject* parent = nullptr);
    ~VerificationController() override;

    bool isRunning() const { return running_; }

    // Algorithms in alsoCompute are computed from the same read pass and returned
    // in VerificationResult::computedHashes; only `algorithm` is verified.
    void start(
        const QString& filePath,
        const QString& expectedChecksum,
        const QString& algorithm,
        qint64 fileSize,
        quint64 jobToken,
        const QStringList& alsoCompute = {},
        iso::IoPolicy ioPolicy = iso::IoPolicy::Buffered);

    void cancel();

  signals:
    void progressUpdated(quint64 jobToken, qint64 bytesRead);
    void finished(quint64 jobToken, const iso::VerificationResult& result);

  private:
    bool running_ = false;
    quint64 activeJobToken_ = 0;
    iso::CancelToken activeCancelToken_;
    QThread* activeWorker_ = nullptr;
};
