#include "gui/app_settings.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace iso {
namespace {

constexpr auto SettingsOrganization = "KaroqDave";
constexpr auto SettingsApplication = "ISO Integrity Check";

} // namespace

AppSettings loadAppSettings()
{
    QSettings settings(QString::fromLatin1(SettingsOrganization), QString::fromLatin1(SettingsApplication));
    AppSettings result;
    if (settings.contains(QStringLiteral("theme"))) {
        result.theme = themeFromSettings(settings.value(QStringLiteral("theme")).toInt());
    }
    if (settings.contains(QStringLiteral("geometry"))) {
        result.geometry = settings.value(QStringLiteral("geometry")).toByteArray();
    }
    result.lastIsoDir = settings.value(QStringLiteral("lastIsoDir")).toString();
    result.lastChecksumDir = settings.value(QStringLiteral("lastChecksumDir")).toString();
    return result;
}

void saveAppSettings(const AppSettings& settings)
{
    QSettings store(QString::fromLatin1(SettingsOrganization), QString::fromLatin1(SettingsApplication));
    store.setValue(QStringLiteral("theme"), themeToSettings(settings.theme));
    store.setValue(QStringLiteral("geometry"), settings.geometry);
    store.setValue(QStringLiteral("lastIsoDir"), settings.lastIsoDir);
    store.setValue(QStringLiteral("lastChecksumDir"), settings.lastChecksumDir);
}

QString browseStartDirectory(const QString& savedDir)
{
    if (!savedDir.isEmpty() && QDir(savedDir).exists()) {
        return savedDir;
    }
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

void rememberBrowseDirectory(QSettings& settings, const QString& key, const QString& filePath)
{
    const QString dir = QFileInfo(filePath).absolutePath();
    if (!dir.isEmpty()) {
        settings.setValue(key, dir);
    }
}

} // namespace iso
