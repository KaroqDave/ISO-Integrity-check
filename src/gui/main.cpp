#include "gui/main_window.h"
#include "gui/theme.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));

    iso::applyTheme(app, iso::Theme::System);

    MainWindow window;
    window.show();

    return app.exec();
}
