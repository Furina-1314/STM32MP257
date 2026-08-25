#include "Salacia_Terminal.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Salacia_Terminal window;
    window.show();
    return app.exec();
}
