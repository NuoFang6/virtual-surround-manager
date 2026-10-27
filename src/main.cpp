#include "frontend_manager.h"
#include "pipewire_manager.h"
#include "tray_icon.h"
#include <KAboutData>
#include <KIconTheme>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QUrl>
#include <QtQml>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <unistd.h>

//
// This checks for an already running application.
// Adapted from EasyEffects: https://github.com/wwmm/easyeffects/blob/master/src/util.cpp
//
static std::unique_ptr<QLockFile> get_lock_file() {
    auto sys_path = std::filesystem::exists("/.flatpak-info") ? QStandardPaths::TempLocation : QStandardPaths::RuntimeLocation;

    auto lock_file = std::make_unique<QLockFile>(QStandardPaths::writableLocation(sys_path) + QStringLiteral("/virtual-surround-manager.lock"));

    lock_file->setStaleLockTime(0);

    bool status = lock_file->tryLock(100);

    if (!status) {
        qDebug("get_lock_file: Could not lock the file: %s", lock_file->fileName().toStdString().c_str());

        switch (lock_file->error()) {
        case QLockFile::NoError:
            break;
        case QLockFile::LockFailedError: {
            qDebug("get_lock_file: Another instance already has the lock");
            break;
        }
        case QLockFile::PermissionError: {
            qDebug("get_lock_file: No permission to create the lock file");
            break;
        }
        case QLockFile::UnknownError: {
            qDebug("get_lock_file: Unknown error");
            break;
        }
        }
    }

    return lock_file;
}

int main(int argc, char *argv[]) {
    // Initialize GUI
    KIconTheme::initTheme();
    QApplication app(argc, argv);

    // Quit cleanly on SIGTERM/SIGINT (logout, manual kill): only a clean exit
    // runs the PipeWire teardown, which releases routed streams. A hard kill
    // leaves their target.object pointing at a dead node and every stream then
    // has to migrate devices on its own - audible as an interruption.
    static int signal_pipe[2] = {-1, -1};
    if (pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) == 0) {
        auto *notifier = new QSocketNotifier(signal_pipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [notifier]() {
            char c;
            while (read(signal_pipe[0], &c, 1) == 1) {
            }
            notifier->setEnabled(false);
            QCoreApplication::quit();
        });

        struct sigaction sa = {};
        sa.sa_handler = [](int) {
            if (signal_pipe[1] >= 0) {
                const char c = 1;
                (void)write(signal_pipe[1], &c, 1);
            }
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
    }

    // App information
    KLocalizedString::setApplicationDomain("virtual-surround-manager");
    QApplication::setOrganizationDomain(QStringLiteral("berny23.de"));
    QApplication::setApplicationName(QStringLiteral("virtual-surround-manager"));
    QApplication::setDesktopFileName(QStringLiteral("de.berny23.virtual_surround_manager"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("de.berny23.virtual_surround_manager")));

    // Set theme
    QApplication::setStyle(QStringLiteral("breeze"));
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    // Credits
    KAboutData aboutData(QStringLiteral("virtual-surround-manager"),
                         i18nc("@title", "Virtual Surround"),
                         QStringLiteral(PROJECT_VERSION),
                         i18nc("@subtitle", "3D sound for headphones"),
                         KAboutLicense::MIT,
                         QStringLiteral("© 2026, Berny23"));
    aboutData.setOrganizationDomain("berny23.de");
    aboutData.setDesktopFileName(QStringLiteral("de.berny23.virtual_surround_manager"));
    aboutData.setProgramLogo(QStringLiteral("de.berny23.virtual_surround_manager"));
    aboutData.setBugAddress("https://github.com/Berny23/virtual-surround-manager/issues");
    aboutData.addComponent(KAboutComponent(
        i18nc("@component", "Contains HRIR WAV files from HeSuVi. © 2021, Matt Gore. See: hesuvi_license.txt"),
        QStringLiteral(""),
        QStringLiteral(""),
        QStringLiteral("https://hesuvi.net"),
        KAboutLicense::MIT));
    aboutData.addAuthor(
        QStringLiteral("Berny23"),
        i18nc("@info:credit", "Developer"),
        QStringLiteral(""),
        QStringLiteral("https://berny23.de"));
    KAboutData::setApplicationData(aboutData);

    // Add about page
    qmlRegisterSingletonType("de.berny23.virtual_surround_manager",
                             1,
                             1,
                             "About",
                             [](QQmlEngine *engine, QJSEngine *) -> QJSValue {
                                 return engine->toScriptValue(KAboutData::applicationData());
                             });

    // Force single instance
    auto lockFile = get_lock_file();
    if (!lockFile->isLocked()) {
        qDebug("Another instance of this application is already running");
        return 0;
    }

    // Create PipeWire and frontend manager
    PipeWireManager pipewire_manager;
    KConfig config(QStringLiteral("virtual-surround-manager"));
    // Stack object: destroyed (in reverse declaration order) before the config
    // and the PipeWire manager it points to, and after the QML engine below
    FrontendManager frontend_manager(&pipewire_manager, &config);

    // Create TrayIcon and expose to QML
    auto tray_icon = new TrayIcon(&app);
    tray_icon->set_frontend_manager(&frontend_manager);
    tray_icon->setup(QStringLiteral("de.berny23.virtual_surround_manager"));
    // Update state in case the user set up auto-enabling virtual surround sound on startup
    tray_icon->update_toggle_action_state();

    // Set tray icon visibility depending on user setting
    tray_icon->set_visibility(frontend_manager.get_startup_ui() != QStringLiteral("hideTray"));

    // Connect some frontend manager signals for updating the tray icon ui state
    QObject::connect(&frontend_manager, &FrontendManager::virtual_surround_enabled_changed, tray_icon, &TrayIcon::update_toggle_action_state);
    QObject::connect(&frontend_manager, &FrontendManager::hrir_wav_file_names_changed, tray_icon, &TrayIcon::update_profile_menu);
    QObject::connect(&frontend_manager, &FrontendManager::hrir_wav_file_name_index_changed, tray_icon, &TrayIcon::update_profile_menu);

    // Add main page and supply context
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("frontendManager"), &frontend_manager);
    engine.rootContext()->setContextProperty(QStringLiteral("trayIcon"), tray_icon);
    engine.loadFromModule("de.berny23.virtual_surround_manager", "Main");

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // Set the main QML window pointer to the TrayIcon
    auto *root_object = engine.rootObjects().value(0);
    if (auto *main_window = qobject_cast<QQuickWindow *>(root_object)) {
        tray_icon->set_main_window(main_window);
    }

    if (frontend_manager.get_startup_ui() == QStringLiteral("showTrayHideWindow"))
        tray_icon->hide_main_window();

    // Show GUI
    return app.exec();
}
