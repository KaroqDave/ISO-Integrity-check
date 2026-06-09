#include "gui/main_window.h"

#include "core/checksum.h"
#include "gui/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEvent>
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
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>
#include <QTextDocument>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSize>
#include <QStyle>
#include <QStyleHints>
#include <QUrl>
#include <QVBoxLayout>
#include <QEventLoop>

#include <exception>

#ifndef ISO_APP_VERSION
#define ISO_APP_VERSION "0.0.0-dev"
#endif

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
    const auto hashes = iso::supportedHashes();
    for (const QString& name : iso::supportedHashNames()) {
        const iso::HashDetails& details = hashes.value(name);
        const QString display = details.legacy ? QStringLiteral("%1 (legacy)").arg(name) : name;
        combo->addItem(display, name);
    }
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
    return QStringLiteral("%1 %2").arg(
        QString::number(value, 'f', suffixIndex >= 3 ? 2 : 1), QString::fromLatin1(suffixes[suffixIndex]));
}

QString formatProgressDetail(qint64 bytesRead, qint64 totalBytes, double bytesPerSecond = -1.0)
{
    if (totalBytes <= 0) {
        return QStringLiteral("%1 read").arg(formatByteSize(bytesRead));
    }

    const int percent = static_cast<int>((bytesRead * 100) / totalBytes);
    QString detail =
        QStringLiteral("%1 / %2 (%3%)").arg(formatByteSize(bytesRead), formatByteSize(totalBytes)).arg(percent);

    if (bytesPerSecond > 0.0) {
        detail += QStringLiteral(" · %1/s").arg(formatByteSize(static_cast<qint64>(bytesPerSecond)));
        if (bytesRead < totalBytes) {
            const qint64 remaining = totalBytes - bytesRead;
            const int secondsLeft = static_cast<int>(remaining / bytesPerSecond);
            if (secondsLeft < 60) {
                detail += QStringLiteral(" · ~%1s left").arg(qMax(1, secondsLeft));
            } else {
                detail += QStringLiteral(" · ~%1m left").arg((secondsLeft + 59) / 60);
            }
        }
    }

    return detail;
}

class DropTargetEventFilter : public QObject {
  public:
    DropTargetEventFilter(MainWindow* window, QWidget* targetSection)
        : QObject(targetSection), window_(window), targetSection_(targetSection)
    {
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        Q_UNUSED(watched);
        switch (event->type()) {
        case QEvent::DragEnter:
            window_->handleSectionDragEnter(static_cast<QDragEnterEvent*>(event), targetSection_);
            return true;
        case QEvent::DragMove:
            window_->handleSectionDragMove(static_cast<QDragMoveEvent*>(event), targetSection_);
            return true;
        case QEvent::Drop:
            window_->handleSectionDrop(static_cast<QDropEvent*>(event), targetSection_);
            return true;
        default:
            return QObject::eventFilter(watched, event);
        }
    }

  private:
    MainWindow* window_ = nullptr;
    QWidget* targetSection_ = nullptr;
};

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

MainWindow::MainWindow(iso::Theme initialTheme, QWidget* parent) : QMainWindow(parent), currentTheme(initialTheme)
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

    connect(
        &verificationController,
        &VerificationController::progressUpdated,
        this,
        [this](quint64 jobToken, qint64 bytesRead) {
            if (jobToken != activeJobToken) {
                return;
            }
            updateProgress(bytesRead);
        });
    connect(
        &verificationController,
        &VerificationController::finished,
        this,
        [this](quint64 jobToken, const iso::VerificationResult& result) { finishVerification(result, jobToken); });

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
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* central = new QWidget;
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

    scroll->setWidget(central);
    setCentralWidget(scroll);
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
        fileSection->installEventFilter(new DropTargetEventFilter(this, fileSection));
    }
    if (inputSection) {
        inputSection->setAcceptDrops(true);
        inputSection->installEventFilter(new DropTargetEventFilter(this, inputSection));
    }
}

void MainWindow::loadSettings()
{
    appSettings = iso::loadAppSettings();
    if (!appSettings.geometry.isEmpty()) {
        restoreGeometry(appSettings.geometry);
    }
}

void MainWindow::saveSettings()
{
    appSettings.theme = currentTheme;
    appSettings.geometry = saveGeometry();
    iso::saveAppSettings(appSettings);
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
    statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    statusLabel->setAccessibleName(QStringLiteral("Verification status"));
    detailLabel = new QLabel(QStringLiteral("Ready"));
    detailLabel->setObjectName(QStringLiteral("footnote"));
    detailLabel->setWordWrap(true);
    detailLabel->setAccessibleName(QStringLiteral("Verification details"));
    mismatchDetailView = new QTextEdit;
    mismatchDetailView->setObjectName(QStringLiteral("mismatchDetail"));
    mismatchDetailView->setReadOnly(true);
    mismatchDetailView->setFrameShape(QFrame::NoFrame);
    mismatchDetailView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mismatchDetailView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mismatchDetailView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mismatchDetailView->document()->setDocumentMargin(0);
    mismatchDetailView->setFixedHeight(0);
    mismatchDetailView->setVisible(false);
    resultLayout->addWidget(statusLabel, 0, Qt::AlignTop);
    resultLayout->addWidget(detailLabel, 0, Qt::AlignTop);
    resultLayout->addWidget(mismatchDetailView, 0, Qt::AlignTop);
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
    auto* warning = new QLabel(QStringLiteral(
        "Only trust checksums from the official operating system or vendor "
        "download page. SHA1 and MD5 are legacy options."));
    warning->setObjectName(QStringLiteral("footnote"));
    warning->setWordWrap(true);
    return warning;
}

void MainWindow::browseIsoFile()
{
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Choose ISO file"),
        iso::browseStartDirectory(appSettings.lastIsoDir),
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
            this, QStringLiteral("ISO file"), QStringLiteral("The selected path is not an existing file."));
        return false;
    }

    setIsoFile(path);
    appSettings.lastIsoDir = QFileInfo(path).absolutePath();
    saveSettings();
    return true;
}

void MainWindow::setIsoFile(const QString& path)
{
    fileEdit->setText(path);
    fileEdit->setToolTip(path);
    setStatus(
        iso::VerificationStatus::Generated, QStringLiteral("ISO selected. Paste or import the matching checksum."));
    updateExpectedValidation();
}

void MainWindow::browseChecksumFile()
{
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Choose checksum file"),
        iso::browseStartDirectory(appSettings.lastChecksumDir),
        QStringLiteral("Checksum files (*.sha256 *.sha512 *.sha1 *.md5 *.txt *SUMS)"));
    if (!selected.isEmpty()) {
        tryImportChecksumFile(selected);
    }
}

bool MainWindow::tryImportChecksumFile(const QString& path)
{
    if (!isChecksumPath(path)) {
        rejectWrongFileType(path, QStringLiteral("a checksum file (.sha256, .sha512, .sha1, .md5, .txt, or *SUMS)"));
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(
            this, QStringLiteral("Checksum file"), QStringLiteral("The selected path is not an existing file."));
        return false;
    }

    importChecksumFile(path);
    appSettings.lastChecksumDir = QFileInfo(path).absolutePath();
    saveSettings();
    return true;
}

void MainWindow::importChecksumFile(const QString& path)
{
    try {
        const auto parsed = iso::loadChecksumFile(path, fileEdit->text().isEmpty() ? QString{} : fileEdit->text());
        setAlgorithm(parsed.algorithm);
        expectedEdit->setText(parsed.checksum);

        const QString source = QFileInfo(path).fileName();
        const QString detail =
            parsed.filename.isEmpty()
                ? QStringLiteral("Imported %1 from %2, line %3.").arg(parsed.algorithm, source).arg(parsed.lineNumber)
                : QStringLiteral("Imported %1 from %2, line %3: %4")
                      .arg(parsed.algorithm, source)
                      .arg(parsed.lineNumber)
                      .arg(parsed.filename);
        setStatus(iso::VerificationStatus::Generated, QStringLiteral("Checksum imported."), detail);
        updateExpectedValidation();
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Checksum file"), QString::fromUtf8(error.what()));
        setStatus(
            iso::VerificationStatus::Error,
            QStringLiteral("Checksum file could not be imported."),
            QString::fromUtf8(error.what()));
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
    const QString algorithm = currentAlgorithm();

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
            this, QStringLiteral("ISO file"), QStringLiteral("The selected path is not an existing ISO file."));
        return;
    }

    verificationFileSize = fileInfo.exists() ? fileInfo.size() : 0;
    activeVerificationSummary = fileInfo.exists()
                                    ? QStringLiteral("Verifying: %1 (%2)...").arg(fileInfo.fileName(), algorithm)
                                    : QStringLiteral("Verifying selected file (%1)...").arg(algorithm);

    const quint64 jobToken = nextJobToken();
    activeJobToken = jobToken;
    activeExpectedChecksum = expectedChecksum;

    setRunning(true);
    setStatus(iso::VerificationStatus::Generated, activeVerificationSummary, QString());
    setComputedHash({});
    clearMismatchHighlight();

    lastProgressBytes = 0;
    progressElapsedTimer.restart();

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

    verificationController.start(filePath, expectedChecksum, algorithm, verificationFileSize, jobToken);
}

void MainWindow::cancelVerification()
{
    if (!verificationRunning) {
        return;
    }

    verificationController.cancel();
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

    if (result.status == iso::VerificationStatus::Mismatch) {
        applyMismatchHighlight(activeExpectedChecksum, result.computedHash);
        QString detail = resultDetail(result);
        const QString mismatchSummary = iso::formatChecksumMismatchSummary(
            iso::checksumMismatchPositions(activeExpectedChecksum, result.computedHash));
        if (!mismatchSummary.isEmpty()) {
            detail = QStringLiteral("%1 %2").arg(detail, mismatchSummary);
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

    double bytesPerSecond = -1.0;
    const qint64 elapsedMs = progressElapsedTimer.elapsed();
    if (elapsedMs > 0 && bytesRead > lastProgressBytes) {
        bytesPerSecond = (static_cast<double>(bytesRead - lastProgressBytes) * 1000.0) / static_cast<double>(elapsedMs);
    }
    lastProgressBytes = bytesRead;
    progressElapsedTimer.restart();

    if (verificationFileSize > 0) {
        const QString detail = formatProgressDetail(bytesRead, verificationFileSize, bytesPerSecond);
        progressBar->setValue(progressBarValueForBytes(bytesRead, verificationFileSize));
        progressBar->setFormat(detail);
        progressBar->setAccessibleDescription(detail);
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

void MainWindow::awaitActiveWorker(int timeoutMs)
{
    if (verificationController.isRunning()) {
        verificationController.cancel();
        QElapsedTimer timer;
        timer.start();
        while (verificationController.isRunning() && timer.elapsed() < timeoutMs) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        }
    }
}

void MainWindow::clearAll()
{
    if (verificationRunning) {
        cancelVerification();
        awaitActiveWorker();
    }

    fileEdit->clear();
    fileEdit->setToolTip({});
    expectedEdit->clear();
    computedEdit->clear();
    algorithmCombo->setCurrentIndex(0);
    clearMismatchHighlight();
    updateExpectedValidation();
    setStatus(
        iso::VerificationStatus::Generated,
        QStringLiteral("Select an ISO file, then paste or import a checksum."),
        QStringLiteral("Ready"));
}

void MainWindow::showAbout()
{
    QMessageBox aboutBox(this);
    aboutBox.setWindowTitle(QStringLiteral("About ISO Integrity Check"));
    aboutBox.setText(QStringLiteral("ISO Integrity Check %1").arg(QString::fromLatin1(ISO_APP_VERSION)));
    aboutBox.setInformativeText(
        QStringLiteral(
            "Created by %1\n\nVerify ISO downloads with SHA512, SHA256, SHA1, and MD5 checksums.\n\nSHA1 "
            "and MD5 are legacy options. Prefer SHA256 or SHA512 from an official source.")
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
    if (currentStatus == iso::VerificationStatus::Mismatch && !activeExpectedChecksum.isEmpty() &&
        computedEdit && !computedEdit->text().isEmpty()) {
        applyMismatchHighlight(activeExpectedChecksum, computedEdit->text());
    }
}

void MainWindow::refreshStatusBadge()
{
    const iso::ColorScheme scheme = iso::resolveColorScheme(currentTheme);
    const iso::Palette& palette = iso::paletteFor(scheme);

    const QColor bg = iso::statusBadgeBackground(currentStatus, palette);
    const QColor fg = iso::statusBadgeText(currentStatus, palette);

    statusLabel->setStyleSheet(QStringLiteral(
                                   "QLabel#statusBadge { background: rgba(%1, %2, %3, %4); color: %5; "
                                   "border-radius: 8px; padding: 8px 12px; font-weight: 700; }")
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
            if (algorithmCombo && currentAlgorithm() != *detected) {
                QSignalBlocker blocker(algorithmCombo);
                setAlgorithm(*detected);
            }
        }
    }

    const QString algorithm = currentAlgorithm();
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
    const iso::ColorScheme scheme = iso::resolveColorScheme(currentTheme);
    const iso::Palette& palette = iso::paletteFor(scheme);
    const QString borderStyle =
        QStringLiteral("QLineEdit { border: 1px solid %1; }").arg(palette.statusMismatch.name(QColor::HexRgb));
    if (expectedEdit) {
        expectedEdit->setStyleSheet(borderStyle);
    }
    if (computedEdit) {
        computedEdit->setStyleSheet(borderStyle);
    }

    if (!mismatchDetailView) {
        return;
    }

    const QString normalizedExpected = iso::normalizeChecksum(expected);
    const QString normalizedComputed = iso::normalizeChecksum(computed);
    const QList<qsizetype> mismatchPositions = iso::checksumMismatchPositions(expected, computed);
    const QString mismatchColor = palette.statusMismatch.name(QColor::HexRgb);

    auto highlightMismatches = [&](const QString& value) -> QString {
        QString html;
        qsizetype mismatchIndex = 0;
        for (qsizetype i = 0; i < value.size(); ++i) {
            while (mismatchIndex < mismatchPositions.size() && mismatchPositions.at(mismatchIndex) < i) {
                ++mismatchIndex;
            }

            const QString character = QString(value.at(i)).toHtmlEscaped();
            if (mismatchIndex < mismatchPositions.size() && mismatchPositions.at(mismatchIndex) == i) {
                html +=
                    QStringLiteral("<span style=\"color:%1;font-weight:700;\">%2</span>").arg(mismatchColor, character);
            } else {
                html += character;
            }
        }
        return html;
    };

    const QString expectedHtml =
        mismatchPositions.isEmpty() ? normalizedExpected.toHtmlEscaped() : highlightMismatches(normalizedExpected);
    const QString computedHtml =
        mismatchPositions.isEmpty() ? normalizedComputed.toHtmlEscaped() : highlightMismatches(normalizedComputed);
    mismatchDetailView->setHtml(QStringLiteral(
                                    "<div style=\"word-wrap:break-word; word-break:break-all;\">"
                                    "<p style=\"margin:0;\"><b>Expected:</b> %1</p>"
                                    "<p style=\"margin:6px 0 0 0;\"><b>Computed:</b> %2</p>"
                                    "</div>")
                                    .arg(expectedHtml, computedHtml));

    const qreal docHeight = mismatchDetailView->document()->size().height();
    mismatchDetailView->setFixedHeight(static_cast<int>(qCeil(docHeight)) + 4);
    mismatchDetailView->setVisible(true);
}

void MainWindow::clearMismatchHighlight()
{
    if (expectedEdit) {
        expectedEdit->setStyleSheet({});
    }
    if (computedEdit) {
        computedEdit->setStyleSheet({});
    }
    if (mismatchDetailView) {
        mismatchDetailView->clear();
        mismatchDetailView->setFixedHeight(0);
        mismatchDetailView->setVisible(false);
    }
}

QString MainWindow::currentAlgorithm() const
{
    if (!algorithmCombo) {
        return {};
    }
    const QString data = algorithmCombo->currentData().toString();
    return data.isEmpty() ? algorithmCombo->currentText() : data;
}

void MainWindow::setAlgorithm(const QString& algorithm)
{
    if (!algorithmCombo) {
        return;
    }
    for (int i = 0; i < algorithmCombo->count(); ++i) {
        if (algorithmCombo->itemData(i).toString() == algorithm) {
            algorithmCombo->setCurrentIndex(i);
            return;
        }
    }
}

QString MainWindow::resultDetail(const iso::VerificationResult& result) const
{
    const QString algorithm = currentAlgorithm();
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
    return lower.endsWith(QStringLiteral(".sha256")) || lower.endsWith(QStringLiteral(".sha512")) ||
           lower.endsWith(QStringLiteral(".sha1")) || lower.endsWith(QStringLiteral(".md5")) ||
           lower.endsWith(QStringLiteral(".txt")) || lower.endsWith(QStringLiteral("sums"));
}

bool MainWindow::acceptDragUrls(const QMimeData* mimeData, QWidget* targetWidget) const
{
    if (!mimeData || !mimeData->hasUrls()) {
        return false;
    }

    for (const QUrl& url : mimeData->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();

        if (targetWidget == fileSection) {
            if (isIsoPath(path)) {
                return true;
            }
            continue;
        }

        if (targetWidget == inputSection) {
            if (isChecksumPath(path)) {
                return true;
            }
            continue;
        }

        if (isIsoPath(path) || isChecksumPath(path)) {
            return true;
        }
    }

    return false;
}

void MainWindow::processDroppedUrls(const QList<QUrl>& urls, QWidget* targetWidget)
{
    QString isoPath;
    QString checksumPath;
    QString firstUnsupported;

    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();
        if (isIsoPath(path) && isoPath.isEmpty()) {
            isoPath = path;
        } else if (isChecksumPath(path) && checksumPath.isEmpty()) {
            checksumPath = path;
        } else if (firstUnsupported.isEmpty()) {
            firstUnsupported = path;
        }
    }

    if (!isoPath.isEmpty() && !checksumPath.isEmpty()) {
        trySetIsoFile(isoPath);
        tryImportChecksumFile(checksumPath);
        return;
    }

    if (targetWidget == fileSection) {
        if (!isoPath.isEmpty()) {
            trySetIsoFile(isoPath);
            return;
        }
        if (!firstUnsupported.isEmpty()) {
            rejectWrongFileType(firstUnsupported, QStringLiteral("an ISO file with the .iso extension"));
        }
        return;
    }

    if (targetWidget == inputSection) {
        if (!checksumPath.isEmpty()) {
            tryImportChecksumFile(checksumPath);
            return;
        }
        if (!firstUnsupported.isEmpty()) {
            rejectWrongFileType(
                firstUnsupported, QStringLiteral("a checksum file (.sha256, .sha512, .sha1, .md5, .txt, or *SUMS)"));
        }
        return;
    }

    if (!isoPath.isEmpty()) {
        trySetIsoFile(isoPath);
        return;
    }
    if (!checksumPath.isEmpty()) {
        tryImportChecksumFile(checksumPath);
        return;
    }
    if (!firstUnsupported.isEmpty()) {
        rejectWrongFileType(
            firstUnsupported,
            QStringLiteral("an ISO file (.iso) or a checksum file (.sha256, .sha512, .sha1, .md5, .txt, or *SUMS)"));
    }
}

void MainWindow::handleSectionDragEnter(QDragEnterEvent* event, QWidget* targetSection)
{
    if (acceptDragUrls(event->mimeData(), targetSection)) {
        event->acceptProposedAction();
    }
}

void MainWindow::handleSectionDragMove(QDragMoveEvent* event, QWidget* targetSection)
{
    if (acceptDragUrls(event->mimeData(), targetSection)) {
        event->acceptProposedAction();
    }
}

void MainWindow::handleSectionDrop(QDropEvent* event, QWidget* targetSection)
{
    if (!event->mimeData()->hasUrls()) {
        return;
    }

    processDroppedUrls(event->mimeData()->urls(), targetSection);
    event->acceptProposedAction();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    QWidget* targetWidget = childAt(event->position().toPoint());
    while (targetWidget && targetWidget != fileSection && targetWidget != inputSection) {
        targetWidget = targetWidget->parentWidget();
    }

    if (acceptDragUrls(event->mimeData(), targetWidget)) {
        event->acceptProposedAction();
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event)
{
    QWidget* targetWidget = childAt(event->position().toPoint());
    while (targetWidget && targetWidget != fileSection && targetWidget != inputSection) {
        targetWidget = targetWidget->parentWidget();
    }

    if (acceptDragUrls(event->mimeData(), targetWidget)) {
        event->acceptProposedAction();
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

    processDroppedUrls(event->mimeData()->urls(), targetWidget);
    event->acceptProposedAction();
}

void MainWindow::handleLaunchArgs(const QStringList& paths, bool autoVerify)
{
    QString isoPath;
    QString checksumPath;

    for (const QString& path : paths) {
        if (isIsoPath(path) && isoPath.isEmpty()) {
            isoPath = path;
        } else if (isChecksumPath(path) && checksumPath.isEmpty()) {
            checksumPath = path;
        }
    }

    if (!isoPath.isEmpty()) {
        trySetIsoFile(isoPath);
    }
    if (!checksumPath.isEmpty()) {
        tryImportChecksumFile(checksumPath);
    }

    if (autoVerify && !fileEdit->text().isEmpty()) {
        startVerification();
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
        awaitActiveWorker();
    }

    saveSettings();
    event->accept();
}
