#include "reviaWindow.h"

#include "Core/crashDiagnostics.h"
#include "Core/exitReporter.h"

#include <QApplication>
#include <QPixmap>
#include <QTabWidget>
#include <QTimer>

#include <exception>
#include <filesystem>
#include <string>

int main(int argc, char** argv)
{
    bool smokeTest = false;
    bool runtimeSmokeTest = false;
    bool runtimeReadySmokeTest = false;
    std::string screenshotPath;
    std::string screenshotTab;
    // Layout has to be checked at more than the one size the window happens to open at.
    int forcedWidth = 0;
    int forcedHeight = 0;
    for (int index = 1; index < argc; ++index)
    {
        smokeTest = smokeTest || std::string(argv[index]) == "--ui-smoke-test";
        runtimeSmokeTest = runtimeSmokeTest ||
            std::string(argv[index]) == "--runtime-smoke-test";
        runtimeReadySmokeTest = runtimeReadySmokeTest ||
            std::string(argv[index]) == "--runtime-ready-smoke-test";
        if (std::string(argv[index]) == "--ui-screenshot" && index + 1 < argc)
        {
            screenshotPath = argv[++index];
            smokeTest = true;
        }
        else if (std::string(argv[index]) == "--ui-screenshot-tab" && index + 1 < argc)
        {
            screenshotTab = argv[++index];
        }
        else if (std::string(argv[index]) == "--ui-size" && index + 1 < argc)
        {
            const std::string size = argv[++index];
            const std::size_t separator = size.find('x');
            if (separator != std::string::npos)
            {
                try
                {
                    forcedWidth = std::stoi(size.substr(0, separator));
                    forcedHeight = std::stoi(size.substr(separator + 1));
                }
                catch (const std::exception&)
                {
                    forcedWidth = 0;
                    forcedHeight = 0;
                }
            }
        }
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

    // Installed before the first window exists, so a failure during construction is
    // still recorded. Until this existed, every kind of fatal ended the same way -- the
    // window simply disappeared -- which is indistinguishable from the user closing it.
    revia::core::CrashDiagnostics::Install("logs");
    // Opens the session ledger and reports the previous run if it never closed its own.
    const std::string unrecordedPrevious = revia::core::ExitReporter::Begin("logs");
    if (!unrecordedPrevious.empty())
    {
        revia::core::CrashDiagnostics::Record("PreviousExit", unrecordedPrevious);
    }
    qInstallMessageHandler([](
        const QtMsgType type,
        const QMessageLogContext&,
        const QString& message)
    {
        // Qt's own diagnostics go to the debugger on Windows, which means nothing at all
        // in a released GUI build. A qFatal aborts the process, so it must not be the one
        // message nobody can read.
        const char* category = type == QtFatalMsg ? "QtFatal"
            : type == QtCriticalMsg ? "QtCritical"
            : type == QtWarningMsg ? "QtWarning" : nullptr;
        if (category != nullptr)
        {
            revia::core::CrashDiagnostics::Record(category, message.toStdString());
        }
    });

    QApplication application(argc, argv);
    QApplication::setApplicationName("Revia");
    QApplication::setOrganizationName("R.E.V.I.A");
    QApplication::setQuitOnLastWindowClosed(smokeTest || runtimeSmokeTest);

    // Windows logging out or shutting down closes the application without any of the
    // ordinary paths running, so it gets its own reason instead of arriving next start
    // as an unexplained termination.
    QObject::connect(&application, &QGuiApplication::commitDataRequest,
        &application, []()
        {
            revia::core::ExitReporter::Record(
                revia::core::ExitReason::SystemShutdown,
                "Windows asked the application to close");
        });

    ReviaWindow window(!smokeTest, !smokeTest && !runtimeSmokeTest);
    if (forcedWidth > 0 && forcedHeight > 0)
    {
        window.resize(forcedWidth, forcedHeight);
    }
    if (!runtimeSmokeTest)
    {
        window.show();
    }
    if (!screenshotTab.empty())
    {
        if (auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("tabs")))
        {
            const QString wanted = QString::fromStdString(screenshotTab);
            for (int index = 0; index < tabs->count(); ++index)
            {
                if (tabs->tabText(index).startsWith(wanted, Qt::CaseInsensitive))
                {
                    tabs->setCurrentIndex(index);
                    break;
                }
            }
        }
    }
    if (!screenshotPath.empty())
    {
        QTimer::singleShot(500, &application, [&application, &window, screenshotPath]()
        {
            const bool saved = window.grab().save(QString::fromStdString(screenshotPath));
            window.hide();
            application.exit(saved ? 0 : 2);
        });
    }
    else if (smokeTest)
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
    const int code = application.exec();
    // Whatever unwound the event loop, the session now has exactly one closing line. If
    // something more specific already recorded itself, this is ignored.
    revia::core::ExitReporter::Record(
        revia::core::ExitReason::EventLoopEnded,
        "exec() returned " + std::to_string(code));
    return code;
}
