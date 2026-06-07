#pragma once

#include "gui/theme.h"

#include <QByteArray>
#include <QString>

class QSettings;

namespace iso {

struct AppSettings {
    Theme theme = Theme::System;
    QByteArray geometry;
    QString lastIsoDir;
    QString lastChecksumDir;
};

AppSettings loadAppSettings();
void saveAppSettings(const AppSettings& settings);

QString browseStartDirectory(const QString& savedDir);
void rememberBrowseDirectory(QSettings& settings, const QString& key, const QString& filePath);

} // namespace iso
