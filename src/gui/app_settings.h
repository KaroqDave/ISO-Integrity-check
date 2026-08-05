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
    // Off by default: reading through the system cache is quicker for the usual
    // case of checking an ISO that was just downloaded and is still resident.
    bool unbufferedReads = false;
};

AppSettings loadAppSettings();
void saveAppSettings(const AppSettings& settings);

QString browseStartDirectory(const QString& savedDir);
void rememberBrowseDirectory(QSettings& settings, const QString& key, const QString& filePath);

} // namespace iso
