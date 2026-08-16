#include "reviaWindow.h"

#include <QApplication>
#include <QTimer>

#include <exception>
#include <filesystem>
#include <string>

int main(int argc, char** argv)
{
    bool smokeTest = false;
    bool runtimeSmokeTest = false;
    bool runtimeReadySmokeTest = false;
    for (int index = 1; index < argc; ++index)
    {
        smokeTest = smokeTest || std::string(argv[index]) == "--ui-smoke-test";
        runtimeSmokeTest = runtimeSmokeTest ||
            std::string(argv[index]) == "--runtime-smoke-test";
        runtimeReadySmokeTest = runtimeReadySmokeTest ||
            std::string(argv[index]) == "--runtime-ready-smoke-test";
    }
    runtimeSmokeTest = runtimeSmokeTest || runtimeReadySmokeTest;

    try
    {
        if (argc > 0)
        {
            const std::filesystem::path executablePath = std::filesystem::absolute(argv[0]);
            std::filesystem::current_path(executablePath.parent_path());
        }
    }
    catch (...)
    {
        // The runtime will surface missing configuration through its normal error state.
    }

    QApplication application(argc, argv);
    QApplication::setApplicationName("Revia");
    QApplication::setOrganizationName("R.E.V.I.A");
    QApplication::setQuitOnLastWindowClosed(smokeTest || runtimeSmokeTest);

    ReviaWindow window(!smokeTest, !smokeTest && !runtimeSmokeTest);
    if (!runtimeSmokeTest)
    {
        window.show();
    }
    if (smokeTest)
    {
        QTimer::singleShot(900, &application, [&application, &window]()
        {
            window.hide();
            application.exit(0);
        });
    }
    else if (runtimeReadySmokeTest)
    {
        auto* readyTimer = new QTimer(&application);
        readyTimer->setInterval(250);
        QObject::connect(readyTimer, &QTimer::timeout, &window, [readyTimer, &window]()
        {
            if (window.IsRuntimeStarted())
            {
                readyTimer->stop();
                window.RequestShutdown();
            }
        });
        readyTimer->start();
        QTimer::singleShot(120000, &window, [&window]() { window.RequestShutdown(); });
    }
    else if (runtimeSmokeTest)
    {
        QTimer::singleShot(1500, &window, [&window]()
        {
            window.RequestShutdown();
        });
    }
    return application.exec();
}
