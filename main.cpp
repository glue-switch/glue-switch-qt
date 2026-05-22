#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("Glue Switch"));
    a.setOrganizationName(QStringLiteral("Glue Switch"));
    a.setWindowIcon(QIcon(QStringLiteral(":/assets/title_logo.png")));

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "glue-switch_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    MainWindow w;
    w.show();
    const int exitCode = QApplication::exec();
    a.removeTranslator(&translator);
    return exitCode;
}
