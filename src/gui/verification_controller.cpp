#include "gui/verification_controller.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QThread>

#include <exception>

VerificationController::VerificationController(QObject* parent) : QObject(parent) {}

VerificationController::~VerificationController()
{
    if (activeWorker_) {
        if (activeCancelToken_) {
            activeCancelToken_->store(true);
        }
        activeWorker_->wait(30000);
    }
}

void VerificationController::start(
    const QString& filePath,
    const QString& expectedChecksum,
    const QString& algorithm,
    qint64 fileSize,
    quint64 jobToken)
{
    if (running_) {
        return;
    }

    running_ = true;
    activeJobToken_ = jobToken;
    activeCancelToken_ = iso::makeCancelToken();

    auto* worker = QThread::create(
        [this, filePath, expectedChecksum, algorithm, jobToken, fileSize, cancelToken = activeCancelToken_]() {
            iso::VerificationResult result;
            QElapsedTimer progressTimer;
            progressTimer.start();
            qint64 lastReportedBytes = -1;

            try {
                iso::ProgressCallback progressCallback =
                    [this, jobToken, fileSize, &progressTimer, &lastReportedBytes](qint64 bytesRead) {
                        if (bytesRead == lastReportedBytes) {
                            return;
                        }
                        const bool forceUpdate = fileSize > 0 && bytesRead >= fileSize;
                        if (!forceUpdate && progressTimer.elapsed() < 100 && lastReportedBytes >= 0) {
                            return;
                        }
                        lastReportedBytes = bytesRead;
                        progressTimer.restart();
                        QMetaObject::invokeMethod(
                            this,
                            [this, jobToken, bytesRead]() {
                                if (jobToken == activeJobToken_) {
                                    emit progressUpdated(jobToken, bytesRead);
                                }
                            },
                            Qt::QueuedConnection);
                    };

                result = iso::verifyChecksum(
                    filePath, expectedChecksum, algorithm, std::move(progressCallback), cancelToken);
            } catch (const std::exception& error) {
                result = {iso::VerificationStatus::Error, QString::fromUtf8(error.what()), {}, std::nullopt};
            }

            QMetaObject::invokeMethod(
                this,
                [this, result, jobToken]() {
                    running_ = false;
                    activeCancelToken_.reset();
                    emit finished(jobToken, result);
                },
                Qt::QueuedConnection);
        });

    activeWorker_ = worker;
    connect(worker, &QThread::finished, this, [this, worker]() {
        if (activeWorker_ == worker) {
            activeWorker_ = nullptr;
        }
        worker->deleteLater();
    });
    worker->start();
}

void VerificationController::cancel()
{
    if (!running_ || !activeCancelToken_) {
        return;
    }
    activeCancelToken_->store(true);
}
