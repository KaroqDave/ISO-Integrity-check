#pragma once

#include "core/verifier.h"
#include "gui/app_settings.h"
#include "gui/theme.h"
#include "gui/verification_controller.h"

#include <QElapsedTimer>
#include <QMainWindow>

class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMimeData;
class QGroupBox;
class QLayout;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QLabel;
class QWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(iso::Theme initialTheme = iso::Theme::System, QWidget* parent = nullptr);

    void handleLaunchArgs(const QStringList& paths, bool autoVerify = false);

    void handleSectionDragEnter(QDragEnterEvent* event, QWidget* targetSection);
    void handleSectionDragMove(QDragMoveEvent* event, QWidget* targetSection);
    void handleSectionDrop(QDropEvent* event, QWidget* targetSection);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildUi();
    void setupShortcuts();
    void setupContextMenus();
    void setupDragAndDrop();
    void loadSettings();
    void saveSettings();
    QLayout* buildHeaderLayout();
    QWidget* buildFileSection();
    QWidget* buildInputSection();
    QLayout* buildActionLayout();
    QWidget* buildResultSection();
    QWidget* buildComputedSection();
    QWidget* buildFooterWarning();
    void browseIsoFile();
    void importChecksumFile(const QString& path);
    void browseChecksumFile();
    void setIsoFile(const QString& path);
    bool trySetIsoFile(const QString& path);
    bool tryImportChecksumFile(const QString& path);
    void rejectWrongFileType(const QString& path, const QString& expectedDescription);
    void onVerifyOrCancelClicked();
    void startVerification();
    void cancelVerification();
    void finishVerification(const iso::VerificationResult& result, quint64 jobToken);
    void setRunning(bool running);
    void updateProgress(qint64 bytesRead);
    void setStatus(iso::VerificationStatus status, const QString& message, const QString& detail = {});
    void setComputedHash(const QString& value);
    void copyComputedHash();
    void copyExpectedChecksum();
    void clearAll();
    void showAbout();
    void toggleTheme();
    void applyCurrentTheme();
    void refreshStatusBadge();
    void updateExpectedValidation(bool autoDetectAlgorithm = true);
    void applyMismatchHighlight(const QString& expected, const QString& computed);
    void clearMismatchHighlight();
    QString resultDetail(const iso::VerificationResult& result) const;
    QString currentAlgorithm() const;
    void setAlgorithm(const QString& algorithm);
    quint64 nextJobToken();
    bool isIsoPath(const QString& path) const;
    bool isChecksumPath(const QString& path) const;
    bool acceptDragUrls(const QMimeData* mimeData, QWidget* targetWidget) const;
    void processDroppedUrls(const QList<QUrl>& urls, QWidget* targetWidget);
    void awaitActiveWorker(int timeoutMs = 30000);

    QLineEdit* fileEdit = nullptr;
    QLineEdit* expectedEdit = nullptr;
    QLineEdit* computedEdit = nullptr;
    QComboBox* algorithmCombo = nullptr;
    QProgressBar* progressBar = nullptr;
    QPushButton* verifyButton = nullptr;
    QPushButton* browseIsoButton = nullptr;
    QPushButton* importButton = nullptr;
    QPushButton* copyExpectedButton = nullptr;
    QPushButton* clearButton = nullptr;
    QPushButton* themeButton = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* detailLabel = nullptr;
    QLabel* expectedHintLabel = nullptr;
    QLabel* mismatchDiffLabel = nullptr;
    QGroupBox* fileSection = nullptr;
    QGroupBox* inputSection = nullptr;

    VerificationController verificationController;
    iso::AppSettings appSettings;
    iso::Theme currentTheme = iso::Theme::System;
    iso::VerificationStatus currentStatus = iso::VerificationStatus::Generated;

    bool verificationRunning = false;
    quint64 activeJobToken = 0;
    quint64 jobTokenCounter = 0;
    qint64 verificationFileSize = 0;
    QString activeVerificationSummary;

    QElapsedTimer progressElapsedTimer;
    qint64 lastProgressBytes = 0;
};
