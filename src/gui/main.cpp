#include "gui/main_window.h"
#include "gui/theme.h"

#include <QApplication>
#include <QIcon>
#include <QSettings>

namespace {

constexpr auto SettingsOrganization = "KaroqDave";
constexpr auto SettingsApplication = "ISO Integrity Check";

iso::Theme loadInitialTheme()
{
    QSettings settings(QString::fromLatin1(SettingsOrganization), QString::fromLatin1(SettingsApplication));
    if (settings.contains(QStringLiteral("theme"))) {
        return iso::themeFromSettings(settings.value(QStringLiteral("theme")).toInt());
    }
    return iso::Theme::System;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QString::fromLatin1(SettingsOrganization));
    QApplication::setApplicationName(QString::fromLatin1(SettingsApplication));

    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));

    const iso::Theme initialTheme = loadInitialTheme();
    iso::applyTheme(app, initialTheme);

    MainWindow window(initialTheme);
    window.show();

    return app.exec();
}
