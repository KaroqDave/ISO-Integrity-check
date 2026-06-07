#pragma once

#include "core/verifier.h"
#include "gui/theme.h"

#include <QMainWindow>

class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QGroupBox;
class QLayout;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QLabel;
class QThread;
class QWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(iso::Theme initialTheme = iso::Theme::System, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
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
    void onVerifyOrCancelClicked();
    void startVerification();
    void cancelVerification();
    void finishVerification(const iso::VerificationResult& result, quint64 jobToken);
    void setRunning(bool running);
    void updateProgress(qint64 bytesRead);
    void setStatus(iso::VerificationStatus status, const QString& message, const QString& detail = {});
    void setComputedHash(const QString& value);
    void copyComputedHash();
    void clearAll();
    void showAbout();
    void toggleTheme();
    void applyCurrentTheme();
    void refreshStatusBadge();
    void updateExpectedValidation();
    void applyMismatchHighlight(const QString& expected, const QString& computed);
    void clearMismatchHighlight();
    QString resultDetail(const iso::VerificationResult& result) const;
    quint64 nextJobToken();
    bool isIsoPath(const QString& path) const;
    bool isChecksumPath(const QString& path) const;
    void handleDroppedPath(const QString& path, QWidget* targetWidget);

    QLineEdit* fileEdit = nullptr;
    QLineEdit* expectedEdit = nullptr;
    QLineEdit* computedEdit = nullptr;
    QComboBox* algorithmCombo = nullptr;
    QProgressBar* progressBar = nullptr;
    QPushButton* verifyButton = nullptr;
    QPushButton* browseIsoButton = nullptr;
    QPushButton* importButton = nullptr;
    QPushButton* clearButton = nullptr;
    QPushButton* themeButton = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* detailLabel = nullptr;
    QLabel* expectedHintLabel = nullptr;
    QGroupBox* fileSection = nullptr;
    QGroupBox* inputSection = nullptr;

    iso::Theme currentTheme = iso::Theme::System;
    iso::VerificationStatus currentStatus = iso::VerificationStatus::Generated;

    bool verificationRunning = false;
    quint64 activeJobToken = 0;
    quint64 jobTokenCounter = 0;
    qint64 verificationFileSize = 0;
    QString activeVerificationSummary;

    QThread* activeWorker = nullptr;
    iso::CancelToken activeCancelToken;
};
