#include "gui/main_window.h"

#include "core/checksum.h"
#include "gui/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSize>
#include <QStyleHints>
#include <QThread>
#include <QVBoxLayout>

#include <exception>

namespace {

constexpr auto AppAuthor = "KaroqDave";
constexpr auto AppProfileUrl = "https://github.com/KaroqDave";

QGroupBox* card(const QString& title)
{
    auto* box = new QGroupBox(title);
    box->setObjectName(QStringLiteral("card"));

    auto* shadow = new QGraphicsDropShadowEffect(box);
    shadow->setBlurRadius(24);
    shadow->setXOffset(0);
    shadow->setYOffset(4);
    shadow->setColor(QColor(15, 23, 42, 40));
    box->setGraphicsEffect(shadow);

    return box;
}

QString themeButtonText(iso::Theme theme)
{
    switch (theme) {
    case iso::Theme::System:
        return QStringLiteral("Theme: Auto");
    case iso::Theme::Light:
        return QStringLiteral("Theme: Light");
    case iso::Theme::Dark:
        return QStringLiteral("Theme: Dark");
    }
    return QStringLiteral("Theme");
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ISO Integrity Check"));
    setMinimumSize(820, 600);
    buildUi();

    if (auto* hints = QApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this]() {
            if (currentTheme == iso::Theme::System) {
                applyCurrentTheme();
            }
        });
    }
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(28, 24, 28, 20);
    mainLayout->setSpacing(16);

    auto* header = new QGridLayout();
    header->setHorizontalSpacing(12);
    auto* title = new QLabel(QStringLiteral("ISO Integrity Check"));
    title->setObjectName(QStringLiteral("title"));
    auto* subtitle = new QLabel(QStringLiteral("Verify ISO downloads with SHA512, SHA256, SHA1, or MD5 checksums."));
    subtitle->setObjectName(QStringLiteral("subtitle"));

    auto* headerButtons = new QHBoxLayout();
    headerButtons->setSpacing(6);
    themeButton = new QPushButton(themeButtonText(currentTheme));
    themeButton->setProperty("variant", "text");
    themeButton->setCursor(Qt::PointingHandCursor);
    connect(themeButton, &QPushButton::clicked, this, &MainWindow::toggleTheme);
    auto* aboutButton = new QPushButton(QStringLiteral("About"));
    aboutButton->setProperty("variant", "text");
    aboutButton->setCursor(Qt::PointingHandCursor);
    connect(aboutButton, &QPushButton::clicked, this, &MainWindow::showAbout);
    headerButtons->addWidget(themeButton);
    headerButtons->addWidget(aboutButton);

    header->addWidget(title, 0, 0);
    header->addLayout(headerButtons, 0, 1, Qt::AlignRight | Qt::AlignTop);
    header->addWidget(subtitle, 1, 0, 1, 2);
    header->setColumnStretch(0, 1);
    mainLayout->addLayout(header);

    auto* fileBox = card(QStringLiteral("ISO file"));
    auto* fileLayout = new QGridLayout(fileBox);
    fileLayout->setSpacing(10);
    fileEdit = new QLineEdit();
    fileEdit->setPlaceholderText(QStringLiteral("Select an .iso file to verify"));
    auto* browseButton = new QPushButton(QStringLiteral("Browse..."));
    browseButton->setProperty("variant", "secondary");
    browseButton->setCursor(Qt::PointingHandCursor);
    connect(browseButton, &QPushButton::clicked, this, &MainWindow::browseIsoFile);
    fileLayout->addWidget(fileEdit, 0, 0);
    fileLayout->addWidget(browseButton, 0, 1);
    fileLayout->setColumnStretch(0, 1);
    mainLayout->addWidget(fileBox);

    auto* inputBox = card(QStringLiteral("Verification input"));
    auto* inputLayout = new QGridLayout(inputBox);
    inputLayout->setHorizontalSpacing(12);
    inputLayout->setVerticalSpacing(12);
    algorithmCombo = new QComboBox();
    algorithmCombo->addItems(iso::supportedHashNames());
    algorithmCombo->setCursor(Qt::PointingHandCursor);
    auto* algorithmView = new QListView(algorithmCombo);
    algorithmView->setFrameShape(QFrame::NoFrame);
    algorithmCombo->setView(algorithmView);
    // Force a uniform popup row height; the list view ignores the stylesheet's
    // min-height when laying out rows, so set it explicitly via the size hint.
    for (int i = 0; i < algorithmCombo->count(); ++i) {
        algorithmCombo->setItemData(i, QSize(0, 32), Qt::SizeHintRole);
    }
    if (QWidget* popup = algorithmView->parentWidget()) {
        // The popup is a top-level window; make only this container transparent and
        // shadow-free so the rounded item view has no square corner artifacts behind
        // it. The rule is scoped by objectName so it does not affect the child view.
        popup->setObjectName(QStringLiteral("comboPopup"));
        popup->setWindowFlag(Qt::FramelessWindowHint, true);
        popup->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        popup->setAttribute(Qt::WA_TranslucentBackground, true);
        popup->setStyleSheet(QStringLiteral("QWidget#comboPopup { background: transparent; }"));
    }
    expectedEdit = new QLineEdit();
    expectedEdit->setPlaceholderText(QStringLiteral("Paste the expected checksum here"));
    auto* importButton = new QPushButton(QStringLiteral("Import checksum file..."));
    importButton->setProperty("variant", "secondary");
    importButton->setCursor(Qt::PointingHandCursor);
    connect(importButton, &QPushButton::clicked, this, &MainWindow::browseChecksumFile);

    auto* hashLabel = new QLabel(QStringLiteral("Hash type"));
    hashLabel->setObjectName(QStringLiteral("fieldLabel"));
    auto* expectedLabel = new QLabel(QStringLiteral("Expected checksum"));
    expectedLabel->setObjectName(QStringLiteral("fieldLabel"));
    inputLayout->addWidget(hashLabel, 0, 0);
    inputLayout->addWidget(algorithmCombo, 0, 1);
    inputLayout->addWidget(importButton, 0, 2, Qt::AlignRight);
    inputLayout->addWidget(expectedLabel, 1, 0);
    inputLayout->addWidget(expectedEdit, 1, 1, 1, 2);
    inputLayout->setColumnStretch(1, 1);
    mainLayout->addWidget(inputBox);

    auto* actionLayout = new QGridLayout();
    actionLayout->setHorizontalSpacing(14);
    progressBar = new QProgressBar();
    progressBar->setRange(0, 1);
    progressBar->setValue(0);
    progressBar->setTextVisible(false);
    verifyButton = new QPushButton(QStringLiteral("Calculate / Verify"));
    verifyButton->setProperty("variant", "primary");
    verifyButton->setMinimumHeight(40);
    verifyButton->setCursor(Qt::PointingHandCursor);
    connect(verifyButton, &QPushButton::clicked, this, &MainWindow::startVerification);
    actionLayout->addWidget(progressBar, 0, 0);
    actionLayout->addWidget(verifyButton, 0, 1);
    actionLayout->setColumnStretch(0, 1);
    mainLayout->addLayout(actionLayout);

    auto* resultBox = card(QStringLiteral("Result"));
    auto* resultLayout = new QVBoxLayout(resultBox);
    resultLayout->setSpacing(10);
    statusLabel = new QLabel(QStringLiteral("Select an ISO file, then paste or import a checksum."));
    statusLabel->setObjectName(QStringLiteral("statusBadge"));
    statusLabel->setWordWrap(true);
    detailLabel = new QLabel(QStringLiteral("Ready"));
    detailLabel->setObjectName(QStringLiteral("footnote"));
    detailLabel->setWordWrap(true);
    resultLayout->addWidget(statusLabel);
    resultLayout->addWidget(detailLabel);
    mainLayout->addWidget(resultBox);

    auto* computedBox = card(QStringLiteral("Computed checksum"));
    auto* computedLayout = new QVBoxLayout(computedBox);
    computedLayout->setSpacing(10);
    computedEdit = new QLineEdit();
    computedEdit->setReadOnly(true);
    computedEdit->setPlaceholderText(QStringLiteral("The computed checksum will appear here"));
    auto* copyButton = new QPushButton(QStringLiteral("Copy computed checksum"));
    copyButton->setProperty("variant", "secondary");
    copyButton->setCursor(Qt::PointingHandCursor);
    connect(copyButton, &QPushButton::clicked, this, &MainWindow::copyComputedHash);
    computedLayout->addWidget(computedEdit);
    computedLayout->addWidget(copyButton, 0, Qt::AlignRight);
    mainLayout->addWidget(computedBox);

    auto* warning = new QLabel(QStringLiteral("Only trust checksums from the official operating system or vendor download page. SHA1 and MD5 are legacy options."));
    warning->setObjectName(QStringLiteral("footnote"));
    warning->setWordWrap(true);
    mainLayout->addWidget(warning);
    mainLayout->addStretch();

    setCentralWidget(central);

    refreshStatusBadge();
}

void MainWindow::browseIsoFile()
{
    const QString selected = QFileDialog::getOpenFileName(this, QStringLiteral("Choose ISO file"), {}, QStringLiteral("ISO files (*.iso);;All files (*.*)"));
    if (!selected.isEmpty()) {
        fileEdit->setText(selected);
        setStatus(iso::VerificationStatus::Generated, QStringLiteral("ISO selected. Paste or import the matching checksum."));
    }
}

void MainWindow::browseChecksumFile()
{
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Choose checksum file"),
        {},
        QStringLiteral("Checksum files (*.sha256 *.sha512 *.sha1 *.md5 *.txt *SUMS);;All files (*.*)"));
    if (selected.isEmpty()) {
        return;
    }

    try {
        const auto parsed = iso::loadChecksumFile(selected, fileEdit->text().isEmpty() ? QString{} : fileEdit->text());
        algorithmCombo->setCurrentText(parsed.algorithm);
        expectedEdit->setText(parsed.checksum);

        const QString source = QFileInfo(selected).fileName();
        const QString detail = parsed.filename.isEmpty()
            ? QStringLiteral("Imported %1 from %2, line %3.").arg(parsed.algorithm, source).arg(parsed.lineNumber)
            : QStringLiteral("Imported %1 from %2, line %3: %4").arg(parsed.algorithm, source).arg(parsed.lineNumber).arg(parsed.filename);
        setStatus(iso::VerificationStatus::Generated, QStringLiteral("Checksum imported."), detail);
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Checksum file"), QString::fromUtf8(error.what()));
        setStatus(iso::VerificationStatus::Error, QStringLiteral("Checksum file could not be imported."), QString::fromUtf8(error.what()));
    }
}

void MainWindow::startVerification()
{
    setRunning(true);
    setStatus(iso::VerificationStatus::Generated, QStringLiteral("Reading file and calculating checksum..."), QStringLiteral("Large ISO files can take a little while."));
    setComputedHash({});

    const QString filePath = fileEdit->text();
    const QString expectedChecksum = expectedEdit->text();
    const QString algorithm = algorithmCombo->currentText();

    auto* worker = QThread::create([this, filePath, expectedChecksum, algorithm]() {
        iso::VerificationResult result;
        try {
            result = iso::verifyChecksum(filePath, expectedChecksum, algorithm);
        } catch (const std::exception& error) {
            result = {iso::VerificationStatus::Error, QStringLiteral("Error: %1").arg(QString::fromUtf8(error.what())), {}, std::nullopt};
        }

        QMetaObject::invokeMethod(this, [this, result]() {
            finishVerification(result);
        }, Qt::QueuedConnection);
    });

    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::finishVerification(const iso::VerificationResult& result)
{
    setRunning(false);
    setStatus(result.status, result.message, resultDetail(result));
    setComputedHash(result.computedHash);
}

void MainWindow::setRunning(bool running)
{
    verifyButton->setEnabled(!running);
    progressBar->setRange(0, running ? 0 : 1);
    if (!running) {
        progressBar->setValue(0);
    }
}

void MainWindow::setStatus(iso::VerificationStatus status, const QString& message, const QString& detail)
{
    currentStatus = status;
    statusLabel->setText(message);
    detailLabel->setText(detail);
    refreshStatusBadge();
}

void MainWindow::setComputedHash(const QString& value)
{
    computedEdit->setText(value);
}

void MainWindow::copyComputedHash()
{
    const QString value = computedEdit->text();
    if (value.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("No checksum"), QStringLiteral("Calculate a checksum first."));
        return;
    }

    QApplication::clipboard()->setText(value);
    setStatus(iso::VerificationStatus::Generated, QStringLiteral("Computed checksum copied to the clipboard."));
}

void MainWindow::showAbout()
{
    QMessageBox::information(
        this,
        QStringLiteral("About ISO Integrity Check"),
        QStringLiteral("ISO Integrity Check\n\nCreated by %1\n%2\n\nVerify ISO downloads with SHA512, SHA256, SHA1, and MD5 checksums.\n\nSHA1 and MD5 are legacy options. Prefer SHA256 or SHA512 from an official source.")
            .arg(QString::fromLatin1(AppAuthor), QString::fromLatin1(AppProfileUrl)));
}

void MainWindow::toggleTheme()
{
    switch (currentTheme) {
    case iso::Theme::System:
        currentTheme = iso::Theme::Light;
        break;
    case iso::Theme::Light:
        currentTheme = iso::Theme::Dark;
        break;
    case iso::Theme::Dark:
        currentTheme = iso::Theme::System;
        break;
    }
    applyCurrentTheme();
}

void MainWindow::applyCurrentTheme()
{
    iso::applyTheme(*qApp, currentTheme);
    if (themeButton) {
        themeButton->setText(themeButtonText(currentTheme));
    }
    refreshStatusBadge();
}

void MainWindow::refreshStatusBadge()
{
    const iso::ColorScheme scheme = iso::resolveColorScheme(currentTheme);
    const iso::Palette& palette = iso::paletteFor(scheme);

    const QColor bg = iso::statusBadgeBackground(currentStatus, palette);
    const QColor fg = iso::statusBadgeText(currentStatus, palette);

    statusLabel->setStyleSheet(QStringLiteral(
        "QLabel#statusBadge { background: rgba(%1, %2, %3, %4); color: %5; border-radius: 8px; padding: 8px 12px; font-weight: 700; }")
        .arg(bg.red())
        .arg(bg.green())
        .arg(bg.blue())
        .arg(bg.alpha())
        .arg(fg.name(QColor::HexRgb)));
}

QString MainWindow::resultDetail(const iso::VerificationResult& result) const
{
    const QString algorithm = algorithmCombo->currentText();
    switch (result.status) {
    case iso::VerificationStatus::Generated:
        return QStringLiteral("Computed %1. Import or paste an official checksum to compare.").arg(algorithm);
    case iso::VerificationStatus::Match:
        return QStringLiteral("Computed %1 equals the expected checksum.").arg(algorithm);
    case iso::VerificationStatus::Mismatch:
        return QStringLiteral("Computed %1 differs from the expected checksum.").arg(algorithm);
    case iso::VerificationStatus::Error:
        return result.computedHash.isEmpty() ? QStringLiteral("The checksum could not be calculated.") : QString{};
    }
    return {};
}
