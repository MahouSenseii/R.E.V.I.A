#include "reviaWindow.h"
#include "runtimeRootArgument.h"

#include "Core/crashDiagnostics.h"
#include "Core/exitReporter.h"
#include "Core/logger.h"
#include "Core/runtimePath.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QPixmap>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
// A restart from Settings starts the new process before the old one has finished closing its
// databases and logs. The new one waits (up to 10 s) for the old one to be gone first.
void WaitForPreviousProcess(const unsigned long processId)
{
#ifdef Q_OS_WIN
    if (processId == 0)
        return;
    if (HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(processId)); previous != nullptr)
    {
        WaitForSingleObject(previous, 10000);
        CloseHandle(previous);
    }
#else
    (void)processId;
#endif
}
}

int main(int argc, char** argv)
{
    bool smokeTest = false;
    bool isolatedUiSmokeTest = false;
    bool runtimeSmokeTest = false;
    bool runtimeReadySmokeTest = false;
    std::string screenshotPath;
    std::string screenshotTab;
    // Layout has to be checked at more than the one size the window happens to open at.
    int forcedWidth = 0;
    int forcedHeight = 0;
    // Starts the runtime, stays alive for a fixed period, then shuts down cleanly.
    //
    // --runtime-ready-smoke-test exits the moment startup reports ready, which is before
    // any background worker has had a chance to do anything. Confirming that the
    // initiative and autonomy loops actually run needs the process to outlive their
    // debounce, and doing that by hand is not a check anyone will repeat.
    int holdSeconds = 0;
    unsigned long restartedFrom = 0;
    revia::desktop::RuntimeRootArgument runtimeRoot;
    for (int index = 1; index < argc; ++index)
    {
        if (std::string(argv[index]).rfind("--after-restart=", 0) == 0)
        {
            try
            {
                restartedFrom = std::stoul(std::string(argv[index]).substr(std::string("--after-restart=").size()));
            }
            catch (const std::exception&)
            {
                restartedFrom = 0;
            }
            continue;
        }
        if (std::string(argv[index]) == "--runtime-root")
        {
            if (index + 1 >= argc ||
                !runtimeRoot.Accept(QString::fromLocal8Bit(argv[++index]).toUtf8().toStdString()))
            {
                std::cerr << runtimeRoot.Error() << '\n';
                return 2;
            }
            continue;
        }
        if (std::string(argv[index]).rfind("--runtime-root=", 0) == 0)
        {
            std::cerr << runtimeRoot.Error() << '\n';
            return 2;
        }
        smokeTest = smokeTest || std::string(argv[index]) == "--ui-smoke-test";
        isolatedUiSmokeTest = isolatedUiSmokeTest || std::string(argv[index]) == "--ui-smoke-test";
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
        else if (std::string(argv[index]) == "--ui-theme" && index + 1 < argc)
        {
            // Read by the theme when the window is built; system, light or dark.
            qputenv("REVIA_THEME", QByteArray(argv[++index]));
        }
        else if (std::string(argv[index]) == "--runtime-hold" && index + 1 < argc)
        {
            try
            {
                holdSeconds = std::stoi(argv[++index]);
            }
            catch (const std::exception&)
            {
                holdSeconds = 0;
            }
            runtimeSmokeTest = true;
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
    WaitForPreviousProcess(restartedFrom);

    if (const std::filesystem::path directory =
            runtimeRoot.Directory(revia::core::ProgramDirectory(argc > 0 ? argv[0] : nullptr));
        !directory.empty())
    {
        // A failure is left to the runtime, which surfaces missing configuration through
        // its normal error state.
        std::error_code error;
        std::filesystem::current_path(directory, error);
        if (error && runtimeRoot.Specified())
        {
            std::cerr << "The explicit runtime root could not be opened.\n";
            return 2;
        }
    }

    std::unique_ptr<QTemporaryDir> smokeDirectory;
    const auto runtimeDirectory = std::filesystem::current_path();
    struct RestoreDirectory
    {
        std::filesystem::path previous;
        ~RestoreDirectory()
        {
            std::error_code error;
            std::filesystem::current_path(previous, error);
        }
    } restore{runtimeDirectory};
    if (isolatedUiSmokeTest && !runtimeSmokeTest)
    {
        smokeDirectory = std::make_unique<QTemporaryDir>();
        if (!smokeDirectory->isValid()) return 2;
        const std::filesystem::path temporaryRoot(smokeDirectory->path().toStdWString());
        std::error_code error;
        std::filesystem::copy(runtimeDirectory / "Config", temporaryRoot / "Config",
            std::filesystem::copy_options::recursive, error);
        if (error) return 2;
        std::filesystem::current_path(temporaryRoot, error);
        if (error || revia::core::RuntimeRoot() != temporaryRoot) return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, smokeDirectory->path());
    }

    // Installed before the first window exists, so a failure during construction is
    // still recorded. Until this existed, every kind of fatal ended the same way -- the
    // window simply disappeared -- which is indistinguishable from the user closing it.
    revia::core::CrashDiagnostics::Install(ReviaLogDirectory());
    // Opens the session ledger and reports the previous run if it never closed its own.
    const std::string unrecordedPrevious = revia::core::ExitReporter::Begin(ReviaLogDirectory());
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
    if (smokeTest && QGuiApplication::platformName() == "offscreen")
    {
        // Offscreen Qt cannot discover Windows fonts for diagnostic captures.
        QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/segoeui.ttf");
        application.setFont(QFont("Segoe UI"));
    }
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

    // The window lives in its own scope: its workers, and the session's databases and logs, are
    // gone before a restart starts the next process.
    int code = 0;
    bool restart = false;
    {
        ReviaWindow window(!smokeTest, !smokeTest && !runtimeSmokeTest);
        if (forcedWidth > 0 && forcedHeight > 0)
        {
            window.resize(forcedWidth, forcedHeight);
        }
        if (!runtimeSmokeTest)
        {
            window.show();
        }
        // "chat", a panel (key or title prefix), "settings" or "settings:<section>".
        QWidget* shown = &window;
        if (!screenshotTab.empty())
        {
            if (QWidget* target = window.ShowScreenshotTarget(QString::fromStdString(screenshotTab)))
                shown = target;
            if (shown != &window && forcedWidth > 0 && forcedHeight > 0)
                shown->resize(forcedWidth, forcedHeight);
        }
        if (!screenshotPath.empty())
        {
            QTimer::singleShot(500, &application, [&application, &window, shown, screenshotPath]()
            {
                const bool saved = shown->grab().save(QString::fromStdString(screenshotPath));
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
        else if (holdSeconds > 0)
        {
            QTimer::singleShot(holdSeconds * 1000, &window, [&window]()
            {
                window.RequestShutdown();
            });
        }
        else if (runtimeSmokeTest)
        {
            QTimer::singleShot(1500, &window, [&window]()
            {
                window.RequestShutdown();
            });
        }
        code = application.exec();
        // Whatever unwound the event loop, the session now has exactly one closing line. If
        // something more specific already recorded itself, this is ignored.
        revia::core::ExitReporter::Record(
            revia::core::ExitReason::EventLoopEnded,
            "exec() returned " + std::to_string(code));
        restart = window.RestartRequested();
    }
    if (restart)
    {
        QStringList arguments = QCoreApplication::arguments().mid(1);
        arguments.erase(std::remove_if(arguments.begin(), arguments.end(),
                            [](const QString& argument) { return argument.startsWith(QStringLiteral("--after-restart=")); }),
            arguments.end());
        arguments << QStringLiteral("--after-restart=%1").arg(QCoreApplication::applicationPid());
        QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments);
    }
    return code;
}
