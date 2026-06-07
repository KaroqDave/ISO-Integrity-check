#include "gui/main_window.h"

#include "core/checksum.h"
#include "gui/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
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
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSize>
#include <QStyle>
#include <QStyleHints>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <exception>

namespace {

constexpr auto AppAuthor = "KaroqDave";
constexpr auto AppProfileUrl = "https://github.com/KaroqDave";
constexpr auto SettingsOrganization = "KaroqDave";
constexpr auto SettingsApplication = "ISO Integrity Check";

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

QLabel* fieldLabel(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("fieldLabel"));
    return label;
}

QPushButton* styledButton(const QString& text, const char* variant)
{
    auto* button = new QPushButton(text);
    button->setProperty("variant", variant);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

void setupAlgorithmCombo(QComboBox* combo)
{
    combo->addItems(iso::supportedHashNames());
    combo->setCursor(Qt::PointingHandCursor);
    auto* algorithmView = new QListView(combo);
    algorithmView->setFrameShape(QFrame::NoFrame);
    combo->setView(algorithmView);
    for (int i = 0; i < combo->count(); ++i) {
        combo->setItemData(i, QSize(0, 32), Qt::SizeHintRole);
    }
    if (QWidget* popup = algorithmView->parentWidget()) {
        popup->setObjectName(QStringLiteral("comboPopup"));
        popup->setWindowFlag(Qt::FramelessWindowHint, true);
        popup->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        popup->setAttribute(Qt::WA_TranslucentBackground, true);
        popup->setStyleSheet(QStringLiteral("QWidget#comboPopup { background: transparent; }"));
    }
}

QString formatByteSize(qint64 bytes)
{
    static const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int suffixIndex = 0;
    while (value >= 1024.0 && suffixIndex < 4) {
        value /= 1024.0;
        ++suffixIndex;
    }

    if (suffixIndex == 0) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', suffixIndex >= 3 ? 2 : 1), QString::fromLatin1(suffixes[suffixIndex]));
}

QString formatProgressDetail(qint64 bytesRead, qint64 totalBytes)
{
    if (totalBytes <= 0) {
        return QStringLiteral("%1 read").arg(formatByteSize(bytesRead));
    }

    const int percent = static_cast<int>((bytesRead * 100) / totalBytes);
    return QStringLiteral("%1 / %2 (%3%)")
        .arg(formatByteSize(bytesRead), formatByteSize(totalBytes))
        .arg(percent);
}

constexpr int ProgressBarScale = 1000;

int progressBarValueForBytes(qint64 bytesRead, qint64 totalBytes)
{
    if (totalBytes <= 0) {
        return 0;
    }
    const qint64 scaled = (bytesRead * ProgressBarScale) / totalBytes;
    return static_cast<int>(qMin(scaled, static_cast<qint64>(ProgressBarScale)));
}

void addLineEditContextMenu(QLineEdit* edit, bool readOnlyMenu = false)
{
    edit->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(edit, &QLineEdit::customContextMenuRequested, edit, [edit, readOnlyMenu](const QPoint& position) {
        QMenu menu(edit);
        if (!readOnlyMenu) {
            menu.addAction(QStringLiteral("Cut"), edit, &QLineEdit::cut);
        }
        menu.addAction(QStringLiteral("Copy"), edit, &QLineEdit::copy);
        if (!readOnlyMenu) {
            menu.addAction(QStringLiteral("Paste"), edit, &QLineEdit::paste);
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("Select All"), edit, &QLineEdit::selectAll);
        menu.exec(edit->mapToGlobal(position));
    });
}

} // namespace

MainWindow::MainWindow(iso::Theme initialTheme, QWidget* parent)
    : QMainWindow(parent)
    , currentTheme(initialTheme)
{
    setWindowTitle(QStringLiteral("ISO Integrity Check"));
    setMinimumSize(820, 600);
    setAcceptDrops(true);
    buildUi();
    setupShortcuts();
    setupContextMenus();
    setupDragAndDrop();
    loadSettings();
    applyCurrentTheme();

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (auto* hints = QApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this]() {
            if (currentTheme == iso::Theme::System) {
                applyCurrentTheme();
            }
        });
    }
#endif
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(28, 24, 28, 20);
    mainLayout->setSpacing(16);

    mainLayout->addLayout(buildHeaderLayout());
    mainLayout->addWidget(buildFileSection());
    mainLayout->addWidget(buildInputSection());
    mainLayout->addLayout(buildActionLayout());
    mainLayout->addWidget(buildResultSection());
    mainLayout->addWidget(buildComputedSection());
    mainLayout->addWidget(buildFooterWarning());
    mainLayout->addStretch();

    setCentralWidget(central);
    refreshStatusBadge();
}

void MainWindow::setupShortcuts()
{
    auto* browseIsoShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+O")), this);
    connect(browseIsoShortcut, &QShortcut::activated, this, &MainWindow::browseIsoFile);

    auto* importChecksumShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")), this);
    connect(importChecksumShortcut, &QShortcut::activated, this, &MainWindow::browseChecksumFile);

    auto* verifyShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(verifyShortcut, &QShortcut::activated, this, &MainWindow::onVerifyOrCancelClicked);

    auto* cancelShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(cancelShortcut, &QShortcut::activated, this, [this]() {
        if (verificationRunning) {
            cancelVerification();
        }
    });
}

void MainWindow::setupContextMenus()
{
    addLineEditContextMenu(fileEdit, true);
    addLineEditContextMenu(expectedEdit);
    addLineEditContextMenu(computedEdit);
}

void MainWindow::setupDragAndDrop()
{
    if (fileSection) {
        fileSection->setAcceptDrops(true);
    }
    if (inputSection) {
        inputSection->setAcceptDrops(true);
    }
}

void MainWindow::loadSettings()
{
    QSettings settings(QString::fromLatin1(SettingsOrganization), QString::fromLatin1(SettingsApplication));
    if (settings.contains(QStringLiteral("geometry"))) {
        restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    }
}

void MainWindow::saveSettings()
{
    QSettings settings(QString::fromLatin1(SettingsOrganization), QString::fromLatin1(SettingsApplication));
    settings.setValue(QStringLiteral("theme"), iso::themeToSettings(currentTheme));
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
}

QLayout* MainWindow::buildHeaderLayout()
{
    auto* header = new QGridLayout();
    header->setHorizontalSpacing(12);

    auto* title = new QLabel(QStringLiteral("ISO Integrity Check"));
    title->setObjectName(QStringLiteral("title"));
    auto* subtitle = new QLabel(QStringLiteral("Verify ISO downloads with SHA512, SHA256, SHA1, or MD5 checksums."));
    subtitle->setObjectName(QStringLiteral("subtitle"));

    auto* headerButtons = new QHBoxLayout();
    headerButtons->setSpacing(6);
    themeButton = styledButton(themeButtonText(currentTheme), "text");
    connect(themeButton, &QPushButton::clicked, this, &MainWindow::toggleTheme);
    auto* aboutButton = styledButton(QStringLiteral("About"), "text");
    connect(aboutButton, &QPushButton::clicked, this, &MainWindow::showAbout);
    clearButton = styledButton(QStringLiteral("Clear"), "text");
    connect(clearButton, &QPushButton::clicked, this, &MainWindow::clearAll);
    headerButtons->addWidget(clearButton);
    headerButtons->addWidget(themeButton);
    headerButtons->addWidget(aboutButton);

    header->addWidget(title, 0, 0);
    header->addLayout(headerButtons, 0, 1, Qt::AlignRight | Qt::AlignTop);
    header->addWidget(subtitle, 1, 0, 1, 2);
    header->setColumnStretch(0, 1);
    return header;
}

QWidget* MainWindow::buildFileSection()
{
    fileSection = card(QStringLiteral("ISO file"));
    auto* fileLayout = new QGridLayout(fileSection);
    fileLayout->setSpacing(10);
    fileEdit = new QLineEdit();
    fileEdit->setReadOnly(true);
    fileEdit->setPlaceholderText(QStringLiteral("Select an .iso file to verify, or drag and drop one here"));
    fileEdit->setAccessibleName(QStringLiteral("ISO file path"));
    fileEdit->setAccessibleDescription(QStringLiteral("Path to the ISO file to verify"));
    browseIsoButton = styledButton(QStringLiteral("&Browse..."), "secondary");
    connect(browseIsoButton, &QPushButton::clicked, this, &MainWindow::browseIsoFile);
    fileLayout->addWidget(fileEdit, 0, 0);
    fileLayout->addWidget(browseIsoButton, 0, 1);
    fileLayout->setColumnStretch(0, 1);
    return fileSection;
}

QWidget* MainWindow::buildInputSection()
{
    inputSection = card(QStringLiteral("Verification input"));
    auto* inputLayout = new QGridLayout(inputSection);
    inputLayout->setHorizontalSpacing(12);
    inputLayout->setVerticalSpacing(12);
    algorithmCombo = new QComboBox();
    setupAlgorithmCombo(algorithmCombo);
    algorithmCombo->setAccessibleName(QStringLiteral("Hash type"));
    expectedEdit = new QLineEdit();
    expectedEdit->setPlaceholderText(QStringLiteral("Paste the expected checksum here"));
    expectedEdit->setAccessibleName(QStringLiteral("Expected checksum"));
    expectedEdit->setAccessibleDescription(QStringLiteral("Official checksum to compare against the computed hash"));
    connect(expectedEdit, &QLineEdit::textChanged, this, [this]() { updateExpectedValidation(true); });
    connect(algorithmCombo, &QComboBox::currentTextChanged, this, [this]() { updateExpectedValidation(false); });
    expectedHintLabel = new QLabel;
    expectedHintLabel->setObjectName(QStringLiteral("footnote"));
    expectedHintLabel->setWordWrap(true);
    importButton = styledButton(QStringLiteral("Import checksum file..."), "secondary");
    connect(importButton, &QPushButton::clicked, this, &MainWindow::browseChecksumFile);
    inputLayout->addWidget(fieldLabel(QStringLiteral("Hash type")), 0, 0);
    inputLayout->addWidget(algorithmCombo, 0, 1);
    inputLayout->addWidget(importButton, 0, 2, Qt::AlignRight);
    inputLayout->addWidget(fieldLabel(QStringLiteral("Expected checksum")), 1, 0);
    inputLayout->addWidget(expectedEdit, 1, 1, 1, 2);
    inputLayout->addWidget(expectedHintLabel, 2, 1, 1, 2);
    inputLayout->setColumnStretch(1, 1);
    return inputSection;
}

QLayout* MainWindow::buildActionLayout()
{
    auto* actionLayout = new QGridLayout();
    actionLayout->setHorizontalSpacing(14);
    progressBar = new QProgressBar();
    progressBar->setRange(0, 1);
    progressBar->setValue(0);
    progressBar->setTextVisible(true);
    progressBar->setMinimumHeight(28);
    progressBar->setAccessibleName(QStringLiteral("Verification progress"));
    verifyButton = styledButton(QStringLiteral("Calculate / Verify"), "primary");
    verifyButton->setMinimumHeight(40);
    verifyButton->setAccessibleName(QStringLiteral("Calculate or verify checksum"));
    connect(verifyButton, &QPushButton::clicked, this, &MainWindow::onVerifyOrCancelClicked);
    actionLayout->addWidget(progressBar, 0, 0);
    actionLayout->addWidget(verifyButton, 0, 1);
    actionLayout->setColumnStretch(0, 1);
    return actionLayout;
}

QWidget* MainWindow::buildResultSection()
{
    auto* resultBox = card(QStringLiteral("Result"));
    auto* resultLayout = new QVBoxLayout(resultBox);
    resultLayout->setSpacing(10);
    statusLabel = new QLabel(QStringLiteral("Select an ISO file, then paste or import a checksum."));
    statusLabel->setObjectName(QStringLiteral("statusBadge"));
    statusLabel->setWordWrap(true);
    statusLabel->setAccessibleName(QStringLiteral("Verification status"));
    detailLabel = new QLabel(QStringLiteral("Ready"));
    detailLabel->setObjectName(QStringLiteral("footnote"));
    detailLabel->setWordWrap(true);
    detailLabel->setAccessibleName(QStringLiteral("Verification details"));
    resultLayout->addWidget(statusLabel);
    resultLayout->addWidget(detailLabel);
    return resultBox;
}

QWidget* MainWindow::buildComputedSection()
{
    auto* computedBox = card(QStringLiteral("Computed checksum"));
    auto* computedLayout = new QVBoxLayout(computedBox);
    computedLayout->setSpacing(10);
    computedEdit = new QLineEdit();
    computedEdit->setReadOnly(true);
    computedEdit->setPlaceholderText(QStringLiteral("The computed checksum will appear here"));
    computedEdit->setAccessibleName(QStringLiteral("Computed checksum"));
    auto* copyButton = styledButton(QStringLiteral("Copy computed checksum"), "secondary");
    connect(copyButton, &QPushButton::clicked, this, &MainWindow::copyComputedHash);
    computedLayout->addWidget(computedEdit);
    computedLayout->addWidget(copyButton, 0, Qt::AlignRight);
    return computedBox;
}

QWidget* MainWindow::buildFooterWarning()
{
    auto* warning = new QLabel(QStringLiteral("Only trust checksums from the official operating system or vendor download page. SHA1 and MD5 are legacy options."));
    warning->setObjectName(QStringLiteral("footnote"));
    warning->setWordWrap(true);
    return warning;
}

void MainWindow::browseIsoFile()
{
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Choose ISO file"),
        {},
        QStringLiteral("ISO files (*.iso)"));
    if (!selected.isEmpty()) {
        trySetIsoFile(selected);
    }
}

void MainWindow::rejectWrongFileType(const QString& path, const QString& expectedDescription)
{
    QMessageBox::warning(
        this,
        QStringLiteral("Unsupported file type"),
        QStringLiteral("\"%1\" is not accepted for this field.\n\nExpected: %2.")
            .arg(QFileInfo(path).fileName(), expectedDescription));
}

bool MainWindow::trySetIsoFile(const QString& path)
{
    if (!isIsoPath(path)) {
        rejectWrongFileType(path, QStringLiteral("an ISO file with the .iso extension"));
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ISO file"),
            QStringLiteral("The selected path is not an existing file."));
        return false;
    }

    setIsoFile(path);
    return true;
}

void MainWindow::setIsoFile(const QString& path)
{
    fileEdit->setText(path);
    fileEdit->setToolTip(path);
    setStatus(iso::VerificationStatus::Generated, QStringLiteral("ISO selected. Paste or import the matching checksum."));
    updateExpectedValidation();
}

void MainWindow::browseChecksumFile()
{
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Choose checksum file"),
        {},
        QStringLiteral("Checksum files (*.sha256 *.sha512 *.sha1 *.md5 *.txt *SUMS)"));
    if (!selected.isEmpty()) {
        tryImportChecksumFile(selected);
    }
}

bool MainWindow::tryImportChecksumFile(const QString& path)
{
    if (!isChecksumPath(path)) {
        rejectWrongFileType(
            path,
            QStringLiteral("a checksum file (.sha256, .sha512, .sha1, .md5, .txt, or *SUMS)"));
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Checksum file"),
            QStringLiteral("The selected path is not an existing file."));
        return false;
    }

    importChecksumFile(path);
    return true;
}

void MainWindow::importChecksumFile(const QString& path)
{
    try {
        const auto parsed = iso::loadChecksumFile(path, fileEdit->text().isEmpty() ? QString{} : fileEdit->text());
        algorithmCombo->setCurrentText(parsed.algorithm);
        expectedEdit->setText(parsed.checksum);

        const QString source = QFileInfo(path).fileName();
        const QString detail = parsed.filename.isEmpty()
            ? QStringLiteral("Imported %1 from %2, line %3.").arg(parsed.algorithm, source).arg(parsed.lineNumber)
            : QStringLiteral("Imported %1 from %2, line %3: %4").arg(parsed.algorithm, source).arg(parsed.lineNumber).arg(parsed.filename);
        setStatus(iso::VerificationStatus::Generated, QStringLiteral("Checksum imported."), detail);
        updateExpectedValidation();
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Checksum file"), QString::fromUtf8(error.what()));
        setStatus(iso::VerificationStatus::Error, QStringLiteral("Checksum file could not be imported."), QString::fromUtf8(error.what()));
    }
}

void MainWindow::onVerifyOrCancelClicked()
{
    if (verificationRunning) {
        cancelVerification();
        return;
    }
    startVerification();
}

void MainWindow::startVerification()
{
    const QString filePath = fileEdit->text();
    const QString expectedChecksum = expectedEdit->text();
    const QString algorithm = algorithmCombo->currentText();

    if (filePath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ISO file"), QStringLiteral("Choose an ISO file first."));
        return;
    }

    if (!isIsoPath(filePath)) {
        rejectWrongFileType(filePath, QStringLiteral("an ISO file with the .iso extension"));
        return;
    }

    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ISO file"),
            QStringLiteral("The selected path is not an existing ISO file."));
        return;
    }

    verificationFileSize = fileInfo.exists() ? fileInfo.size() : 0;
    activeVerificationSummary = fileInfo.exists()
        ? QStringLiteral("Verifying: %1 (%2)...").arg(fileInfo.fileName(), algorithm)
        : QStringLiteral("Verifying selected file (%1)...").arg(algorithm);

    const quint64 jobToken = nextJobToken();
    activeJobToken = jobToken;
    activeCancelToken = iso::makeCancelToken();

    setRunning(true);
    setStatus(iso::VerificationStatus::Generated, activeVerificationSummary, QString());
    setComputedHash({});
    clearMismatchHighlight();

    if (verificationFileSize > 0) {
        // QProgressBar uses int for its range; scale to 0–1000 so files larger
        // than ~2 GB still show an accurate percentage via the custom format text.
        progressBar->setRange(0, ProgressBarScale);
        progressBar->setValue(0);
        progressBar->setFormat(formatProgressDetail(0, verificationFileSize));
    } else {
        progressBar->setRange(0, 0);
        progressBar->setFormat(QStringLiteral("Calculating..."));
    }

    auto* worker = QThread::create([this, filePath, expectedChecksum, algorithm, jobToken, cancelToken = activeCancelToken]() {
        iso::VerificationResult result;
        try {
            iso::ProgressCallback progressCallback = [this, jobToken](qint64 bytesRead) {
                QMetaObject::invokeMethod(
                    this,
                    [this, jobToken, bytesRead]() {
                        if (jobToken == activeJobToken) {
                            updateProgress(bytesRead);
                        }
                    },
                    Qt::QueuedConnection);
            };
            result = iso::verifyChecksum(filePath, expectedChecksum, algorithm, std::move(progressCallback), cancelToken);
        } catch (const std::exception& error) {
            result = {iso::VerificationStatus::Error, QStringLiteral("Error: %1").arg(QString::fromUtf8(error.what())), {}, std::nullopt};
        }

        QMetaObject::invokeMethod(
            this,
            [this, result, jobToken]() {
                finishVerification(result, jobToken);
            },
            Qt::QueuedConnection);
    });

    activeWorker = worker;
    connect(worker, &QThread::finished, this, [this, worker]() {
        if (activeWorker == worker) {
            activeWorker = nullptr;
        }
        worker->deleteLater();
    });
    worker->start();
}

void MainWindow::cancelVerification()
{
    if (!verificationRunning || !activeCancelToken) {
        return;
    }

    activeCancelToken->store(true);
    setStatus(
        iso::VerificationStatus::Cancelled,
        QStringLiteral("Cancelling verification..."),
        QStringLiteral("Waiting for the current read to finish."));
}

void MainWindow::finishVerification(const iso::VerificationResult& result, quint64 jobToken)
{
    if (jobToken != activeJobToken) {
        return;
    }

    setRunning(false);
    activeCancelToken.reset();

    if (result.status == iso::VerificationStatus::Mismatch) {
        applyMismatchHighlight(expectedEdit->text(), result.computedHash);
        QString detail = resultDetail(result);
        const QString normalizedExpected = iso::normalizeChecksum(expectedEdit->text());
        const QString normalizedComputed = iso::normalizeChecksum(result.computedHash);
        const qsizetype length = qMin(normalizedExpected.size(), normalizedComputed.size());
        qsizetype firstDiff = -1;
        for (qsizetype i = 0; i < length; ++i) {
            if (normalizedExpected.at(i) != normalizedComputed.at(i)) {
                firstDiff = i;
                break;
            }
        }
        if (firstDiff < 0 && normalizedExpected.size() != normalizedComputed.size()) {
            firstDiff = length;
        }
        if (firstDiff >= 0) {
            detail = QStringLiteral("%1 First difference at character %2 (1-based).")
                .arg(detail)
                .arg(firstDiff + 1);
        }
        setStatus(result.status, result.message, detail);
    } else {
        clearMismatchHighlight();
        setStatus(result.status, result.message, resultDetail(result));
    }
    if (!result.computedHash.isEmpty()) {
        setComputedHash(result.computedHash);
    }
}

void MainWindow::setRunning(bool running)
{
    verificationRunning = running;
    verifyButton->setText(running ? QStringLiteral("Cancel") : QStringLiteral("Calculate / Verify"));
    verifyButton->setProperty("variant", running ? "secondary" : "primary");
    verifyButton->style()->unpolish(verifyButton);
    verifyButton->style()->polish(verifyButton);

    fileEdit->setEnabled(!running);
    expectedEdit->setEnabled(!running);
    computedEdit->setEnabled(!running);
    algorithmCombo->setEnabled(!running);
    browseIsoButton->setEnabled(!running);
    importButton->setEnabled(!running);
    clearButton->setEnabled(!running);

    if (!running) {
        progressBar->setRange(0, 1);
        progressBar->setValue(0);
        progressBar->setFormat(QString());
        verificationFileSize = 0;
    }
}

void MainWindow::updateProgress(qint64 bytesRead)
{
    if (!verificationRunning) {
        return;
    }

    if (verificationFileSize > 0) {
        progressBar->setValue(progressBarValueForBytes(bytesRead, verificationFileSize));
        progressBar->setFormat(formatProgressDetail(bytesRead, verificationFileSize));
        progressBar->setAccessibleDescription(formatProgressDetail(bytesRead, verificationFileSize));
    }
}

void MainWindow::setStatus(iso::VerificationStatus status, const QString& message, const QString& detail)
{
    currentStatus = status;
    const QString prefixed = iso::statusBadgePrefix(status) + iso::formatStatusMessage(status, message);
    statusLabel->setText(prefixed);
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
    detailLabel->setText(QStringLiteral("Computed checksum copied to the clipboard."));
}

void MainWindow::clearAll()
{
    if (verificationRunning) {
        cancelVerification();
    }

    fileEdit->clear();
    fileEdit->setToolTip({});
    expectedEdit->clear();
    computedEdit->clear();
    algorithmCombo->setCurrentIndex(0);
    clearMismatchHighlight();
    updateExpectedValidation();
    setStatus(iso::VerificationStatus::Generated, QStringLiteral("Select an ISO file, then paste or import a checksum."), QStringLiteral("Ready"));
}

void MainWindow::showAbout()
{
    QMessageBox aboutBox(this);
    aboutBox.setWindowTitle(QStringLiteral("About ISO Integrity Check"));
    aboutBox.setText(QStringLiteral("ISO Integrity Check"));
    aboutBox.setInformativeText(
        QStringLiteral("Created by %1\n\nVerify ISO downloads with SHA512, SHA256, SHA1, and MD5 checksums.\n\nSHA1 and MD5 are legacy options. Prefer SHA256 or SHA512 from an official source.")
            .arg(QString::fromLatin1(AppAuthor)));
    aboutBox.setStandardButtons(QMessageBox::Ok);
    QPushButton* githubButton = aboutBox.addButton(QStringLiteral("Open GitHub"), QMessageBox::ActionRole);
    aboutBox.exec();
    if (aboutBox.clickedButton() == githubButton) {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(AppProfileUrl)));
    }
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
    saveSettings();
}

void MainWindow::applyCurrentTheme()
{
    iso::applyTheme(*qApp, currentTheme);
    if (themeButton) {
        themeButton->setText(themeButtonText(currentTheme));
    }
    refreshStatusBadge();
    updateExpectedValidation();
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

void MainWindow::updateExpectedValidation(bool autoDetectAlgorithm)
{
    if (!expectedHintLabel) {
        return;
    }

    const QString value = expectedEdit ? expectedEdit->text().trimmed() : QString{};
    if (value.isEmpty()) {
        expectedHintLabel->clear();
        expectedHintLabel->setStyleSheet({});
        return;
    }

    if (autoDetectAlgorithm) {
        if (const auto detected = iso::algorithmFromChecksumLength(value.size())) {
            if (algorithmCombo && algorithmCombo->currentText() != *detected) {
                QSignalBlocker blocker(algorithmCombo);
                algorithmCombo->setCurrentText(*detected);
            }
        }
    }

    const QString algorithm = algorithmCombo ? algorithmCombo->currentText() : QString{};
    if (const auto error = iso::validateExpectedChecksum(value, algorithm)) {
        const iso::ColorScheme scheme = iso::resolveColorScheme(currentTheme);
        const iso::Palette& palette = iso::paletteFor(scheme);
        expectedHintLabel->setText(*error);
        expectedHintLabel->setStyleSheet(QStringLiteral("color: %1;").arg(palette.statusError.name(QColor::HexRgb)));
        return;
    }

    const iso::ColorScheme scheme = iso::resolveColorScheme(currentTheme);
    const iso::Palette& palette = iso::paletteFor(scheme);
    expectedHintLabel->setText(QStringLiteral("Checksum format looks valid for %1.").arg(algorithm));
    expectedHintLabel->setStyleSheet(QStringLiteral("color: %1;").arg(palette.statusMatch.name(QColor::HexRgb)));
}

void MainWindow::applyMismatchHighlight(const QString& expected, const QString& computed)
{
    Q_UNUSED(expected);
    Q_UNUSED(computed);

    const iso::ColorScheme scheme = iso::resolveColorScheme(currentTheme);
    const iso::Palette& palette = iso::paletteFor(scheme);
    computedEdit->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid %1; }").arg(palette.statusMismatch.name(QColor::HexRgb)));
}

void MainWindow::clearMismatchHighlight()
{
    if (computedEdit) {
        computedEdit->setStyleSheet({});
    }
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
    case iso::VerificationStatus::Cancelled:
        return QStringLiteral("Verification was stopped before completion.");
    case iso::VerificationStatus::Error:
        return result.computedHash.isEmpty() ? QStringLiteral("The checksum could not be calculated.") : QString{};
    }
    return {};
}

quint64 MainWindow::nextJobToken()
{
    return ++jobTokenCounter;
}

bool MainWindow::isIsoPath(const QString& path) const
{
    return path.endsWith(QStringLiteral(".iso"), Qt::CaseInsensitive);
}

bool MainWindow::isChecksumPath(const QString& path) const
{
    const QString lower = path.toLower();
    return lower.endsWith(QStringLiteral(".sha256"))
        || lower.endsWith(QStringLiteral(".sha512"))
        || lower.endsWith(QStringLiteral(".sha1"))
        || lower.endsWith(QStringLiteral(".md5"))
        || lower.endsWith(QStringLiteral(".txt"))
        || lower.endsWith(QStringLiteral("sums"));
}

void MainWindow::handleDroppedPath(const QString& path, QWidget* targetWidget)
{
    if (targetWidget == fileSection) {
        trySetIsoFile(path);
        return;
    }

    if (targetWidget == inputSection) {
        tryImportChecksumFile(path);
        return;
    }

    // Drop landed outside a card: route by file type only.
    if (isIsoPath(path)) {
        trySetIsoFile(path);
    } else if (isChecksumPath(path)) {
        tryImportChecksumFile(path);
    } else {
        rejectWrongFileType(path, QStringLiteral("an ISO file (.iso) or a checksum file (.sha256, .sha512, .sha1, .md5, .txt, or *SUMS)"));
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        return;
    }

    QWidget* targetWidget = childAt(event->position().toPoint());
    while (targetWidget && targetWidget != fileSection && targetWidget != inputSection) {
        targetWidget = targetWidget->parentWidget();
    }

    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();

        if (targetWidget == fileSection) {
            if (isIsoPath(path)) {
                event->acceptProposedAction();
                return;
            }
            continue;
        }

        if (targetWidget == inputSection) {
            if (isChecksumPath(path)) {
                event->acceptProposedAction();
                return;
            }
            continue;
        }

        if (isIsoPath(path) || isChecksumPath(path)) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        return;
    }

    QWidget* targetWidget = childAt(event->position().toPoint());
    while (targetWidget && targetWidget != fileSection && targetWidget != inputSection) {
        targetWidget = targetWidget->parentWidget();
    }

    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();

        if (targetWidget == fileSection) {
            if (isIsoPath(path)) {
                trySetIsoFile(path);
                event->acceptProposedAction();
                return;
            }
            rejectWrongFileType(path, QStringLiteral("an ISO file with the .iso extension"));
            return;
        }

        if (targetWidget == inputSection) {
            if (isChecksumPath(path)) {
                tryImportChecksumFile(path);
                event->acceptProposedAction();
                return;
            }
            rejectWrongFileType(
                path,
                QStringLiteral("a checksum file (.sha256, .sha512, .sha1, .md5, .txt, or *SUMS)"));
            return;
        }

        if (isIsoPath(path) || isChecksumPath(path)) {
            handleDroppedPath(path, targetWidget);
            event->acceptProposedAction();
            return;
        }

        rejectWrongFileType(path, QStringLiteral("an ISO file (.iso) or a checksum file (.sha256, .sha512, .sha1, .md5, .txt, or *SUMS)"));
        return;
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (verificationRunning) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("Verification in progress"),
            QStringLiteral("A verification is still running. Cancel it and close?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }

        cancelVerification();
        if (activeWorker) {
            activeWorker->wait(30000);
        }
    }

    saveSettings();
    event->accept();
}
