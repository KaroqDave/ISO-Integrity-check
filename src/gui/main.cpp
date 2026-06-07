#include "gui/main_window.h"
#include "gui/theme.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    iso::applyTheme(app, iso::Theme::System);

    MainWindow window;
    window.show();

    return app.exec();
}
