#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("Glue Switch"));
    a.setOrganizationName(QStringLiteral("Glue Switch"));
    a.setWindowIcon(QIcon(QStringLiteral(":/assets/title_logo.png")));

    MainWindow w;
    w.show();
    return QApplication::exec();
}
