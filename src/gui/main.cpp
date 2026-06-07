#include "gui/app_settings.h"
#include "gui/main_window.h"
#include "gui/theme.h"

#include <QApplication>
#include <QIcon>

namespace {

struct LaunchOptions {
    QStringList paths;
    bool autoVerify = false;
};

LaunchOptions parseLaunchOptions(const QStringList& arguments)
{
    LaunchOptions options;
    for (const QString& arg : arguments) {
        if (arg == QStringLiteral("--verify")) {
            options.autoVerify = true;
        } else if (!arg.startsWith(QLatin1Char('-'))) {
            options.paths.append(arg);
        }
    }
    return options;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("KaroqDave"));
    QApplication::setApplicationName(QStringLiteral("ISO Integrity Check"));

    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));

    const iso::AppSettings settings = iso::loadAppSettings();
    iso::applyTheme(app, settings.theme);

    MainWindow window(settings.theme);
    const LaunchOptions launchOptions = parseLaunchOptions(app.arguments());
    if (!launchOptions.paths.isEmpty()) {
        window.handleLaunchArgs(launchOptions.paths, launchOptions.autoVerify);
    }
    window.show();

    return app.exec();
}
