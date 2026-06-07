#pragma once

#include "core/verifier.h"
#include "gui/theme.h"

#include <QMainWindow>

class QComboBox;
class QLayout;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QLabel;
class QWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildUi();
    QLayout* buildHeaderLayout();
    QWidget* buildFileSection();
    QWidget* buildInputSection();
    QLayout* buildActionLayout();
    QWidget* buildResultSection();
    QWidget* buildComputedSection();
    QWidget* buildFooterWarning();
    void browseIsoFile();
    void browseChecksumFile();
    void startVerification();
    void finishVerification(const iso::VerificationResult& result);
    void setRunning(bool running);
    void setStatus(iso::VerificationStatus status, const QString& message, const QString& detail = {});
    void setComputedHash(const QString& value);
    void copyComputedHash();
    void showAbout();
    void toggleTheme();
    void applyCurrentTheme();
    void refreshStatusBadge();
    QString resultDetail(const iso::VerificationResult& result) const;

    QLineEdit* fileEdit = nullptr;
    QLineEdit* expectedEdit = nullptr;
    QLineEdit* computedEdit = nullptr;
    QComboBox* algorithmCombo = nullptr;
    QProgressBar* progressBar = nullptr;
    QPushButton* verifyButton = nullptr;
    QPushButton* themeButton = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* detailLabel = nullptr;

    iso::Theme currentTheme = iso::Theme::System;
    iso::VerificationStatus currentStatus = iso::VerificationStatus::Generated;
};
