#include "frontend_manager.h"
#include "pipewire_manager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#ifdef IS_FLATPAK
#include <libportal-qt6/portal-qt6.h>
#include <libportal/background.h>
#endif

FrontendManager::FrontendManager(PipeWireManager *pipewire_manager, KConfig *config, QObject *parent) : QObject(parent) {
    m_pipewire_manager = pipewire_manager;
    m_config = config;
    m_config_settings = m_config->group(QStringLiteral("Settings"));
    connect(m_pipewire_manager, &PipeWireManager::error_occurred, this, &FrontendManager::set_error_message);

    // Initialize UI values

    load_hrir_wav_files();

    // Remove the channel setting of versions that still supported 5.1 output
    if (m_config_settings.hasKey("channels")) {
        m_config_settings.deleteEntry("channels");
        m_config->sync();
    }

    m_startup_ui = m_config_settings.readEntry("startup_ui", QString());
    // Fallback for malformed config value
    QStringList allowed_startup_ui_values = {QStringLiteral("showTray"), QStringLiteral("showTrayHideWindow"), QStringLiteral("hideTray")};
    if (!allowed_startup_ui_values.contains(m_startup_ui)) {
        m_startup_ui = QStringLiteral("hideTray"); // Hide by default because not all desktop environments support tray icons out of the box
        m_config_settings.writeEntry("startup_ui", m_startup_ui);
        m_config->sync();
    }

    m_autostart_enabled = m_config_settings.readEntry("autostart_enabled", bool());

    m_upmix_enabled = m_config_settings.readEntry("upmix_enabled", bool());

    m_virtual_surround_auto_enabled = m_config_settings.readEntry("virtual_surround_auto_enabled", bool());
    if (m_virtual_surround_auto_enabled) {
        // This line gets intentionally skipped if auto-enabling is disabled
        set_virtual_surround_enabled(true);
    }

    // Show why enabling virtual surround sound is impossible on a broken PipeWire connection
    if (!m_pipewire_manager->is_valid()) {
        set_error_message(QStringLiteral("Failed connecting to PipeWire audio service."));
    }
}

bool FrontendManager::does_hrir_wav_file_exist(const QString &file_path) {
    if (!QFile(file_path).exists()) {
        qWarning("does_hrir_wav_file_exist: HRIR file does not exist: %s", file_path.toStdString().c_str());
        set_error_message(QStringLiteral("Selected HRIR file does not exist in your filesystem."));
        return false;
    }
    return true;
}

bool FrontendManager::create_module() {
    // A fresh attempt invalidates any error message from a previous failure
    set_error_message(QString());

    if (m_hrir_wav_file_names.isEmpty()) {
        qWarning("create_module: No virtualization profiles available");
        set_error_message(QStringLiteral("No virtualization profiles found. Add HRIR WAV files to the folder below and press Reload."));
        return false;
    }

    QString path = m_hrir_wav_file_paths.value(m_hrir_wav_file_name_index);
    if (!does_hrir_wav_file_exist(path))
        return false;
    if (!m_pipewire_manager->create_virtual_surround_module(path.toStdString(), m_upmix_enabled))
        return false;
    if (m_virtual_surround_enabled)
        m_pipewire_manager->enable_routing();
    return true;
}

void FrontendManager::rebuild_module() {
    // First, disable routing to prevent disruption of playing audio
    if (m_virtual_surround_enabled)
        m_pipewire_manager->disable_routing();

    // Removing waits for the server to tear the module down (no blind sleeps)
    m_pipewire_manager->remove_virtual_surround_module();

    // Keep the enabled state truthful when the module could not be recreated
    if (!create_module() && m_virtual_surround_enabled) {
        m_virtual_surround_enabled = false;
        Q_EMIT virtual_surround_enabled_changed();
    }
}

bool FrontendManager::get_virtual_surround_enabled() const {
    return m_virtual_surround_enabled;
}

void FrontendManager::set_virtual_surround_enabled(bool value) {
    if (m_virtual_surround_enabled == value)
        return;

    if (value) {
        m_virtual_surround_enabled = true;
        // Roll back on failure so the UI and tray do not claim to be enabled
        if (!create_module())
            m_virtual_surround_enabled = false;
    } else {
        m_virtual_surround_enabled = false;
        m_pipewire_manager->disable_routing();
    }

    Q_EMIT virtual_surround_enabled_changed();
}

QString FrontendManager::get_error_message() const {
    return m_error_message;
}

void FrontendManager::set_error_message(const QString &message) {
    if (m_error_message == message)
        return;
    m_error_message = message;
    Q_EMIT error_message_changed();
}

QStringList FrontendManager::get_hrir_wav_file_names() const {
    return m_hrir_wav_file_names;
}

int FrontendManager::get_hrir_wav_file_name_index() const {
    return m_hrir_wav_file_name_index;
}

void FrontendManager::set_hrir_wav_file_name_index(int index) {
    // Check if the index is valid
    if (index < 0 || index >= m_hrir_wav_file_names.size())
        return;
    if (m_hrir_wav_file_name_index == index)
        return;

    m_hrir_wav_file_name_index = index;

    rebuild_module();

    // Write to config file
    m_config_settings.writeEntry("hrir_wav_file_path", m_hrir_wav_file_paths.value(index));
    m_config->sync();

    Q_EMIT hrir_wav_file_name_index_changed();
}

void FrontendManager::load_hrir_wav_files() {
    // Check first if a path has already been selected by the user
    QString old_path = m_hrir_wav_file_paths.value(m_hrir_wav_file_name_index);

    // Read from config file if nothing has been selected in current session
    if (old_path.isEmpty()) {
        old_path = m_config_settings.readEntry("hrir_wav_file_path", QString());
    }

    // Get all data directories, ordered by priority, first "~/.local/share/virtual-surround-manager", then "/usr/share/virtual-surround-manager"
    // In Flatpak: "~/.var/app/de.berny23.virtual_surround_manager/data/virtual-surround-manager", then (the PipeWire-unusable) "/app/share/virtual-surround-manager"
    QStringList data_dirs = QStandardPaths::standardLocations(QStandardPaths::AppDataLocation);

#ifdef IS_FLATPAK
    QDir internal_path(QStringLiteral("/app/share/virtual-surround-manager") + hrir_wav_subpath);
#endif

#ifdef IS_APPIMAGE
    // Read path from data_dirs variable because files are in a random folder inside /tmp
    QDir internal_path(data_dirs.value(1) + hrir_wav_subpath);
#endif

#if defined(IS_FLATPAK) || defined(IS_APPIMAGE)
    // PipeWire requires an absolut path on the host for loading filter chains!
    // The user-writable path is OK, since Qt already returns the full path, e. g. "/home/berny23/.var/app/de.berny23.virtual_surround_manager/data/virtual-surround-manager"
    // However, internal AppImage or Flatpak data folders like "/app/share/virtual-surround-manager" either cannot be used here at all or are temporary.
    // Because a Flatpak can be installed either in system or user mode, we don't know the exact host path. AppImages generally use temporary folders.
    // So we copy the files to the user-writable folder instead, without overwriting.
    if (internal_path.exists()) {
        QString writable_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + hrir_wav_subpath;

        QDir dir(writable_path);
        if (!dir.exists())
            dir.mkpath(QStringLiteral("."));

        internal_path.setNameFilters(QStringList({QStringLiteral("*.wav")}));
        for (const QFileInfo &file : internal_path.entryInfoList(QDir::Files)) {
            if (QFile(writable_path + QStringLiteral("/") + file.fileName()).exists())
                continue;

            QFile::copy(file.absoluteFilePath(), writable_path + QStringLiteral("/") + file.fileName());
        }
        qDebug("load_hrir_wav_files: Copied files from '%s' to '%s'", internal_path.absolutePath().toStdString().c_str(), writable_path.toStdString().c_str());
    }
#endif

    QStringList names;
    QStringList paths;

    for (const QString &path : data_dirs) {
        QDir dir(path + hrir_wav_subpath);
        if (!dir.exists())
            continue;

        // Get all .wav files in the directory, skip duplicates because the priority is user-first
        dir.setNameFilters(QStringList({QStringLiteral("*.wav")}));
        for (const QFileInfo &file : dir.entryInfoList(QDir::Files)) {
            if (names.contains(file.fileName()))
                continue;

            names.append(file.fileName());
            paths.append(file.absoluteFilePath());
        }
    }

    m_hrir_wav_file_names = names;
    m_hrir_wav_file_paths = paths;

    Q_EMIT hrir_wav_file_names_changed();

    // Try to restore the old file selection if it still exists
    int new_index = 0;
    if (!old_path.isEmpty())
        new_index = m_hrir_wav_file_paths.indexOf(old_path);

    // Clamp the selection so it always points at an existing file
    if (new_index < 0 || new_index >= m_hrir_wav_file_paths.size())
        new_index = 0;

    if (new_index != m_hrir_wav_file_name_index) {
        m_hrir_wav_file_name_index = new_index;
        Q_EMIT hrir_wav_file_name_index_changed();
        qDebug("Restored previous file selection");
    }
}

void FrontendManager::open_hrir_wav_folder() {
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + hrir_wav_subpath;

    // Create user data path if not exists
    QDir dir(path);
    if (!dir.exists())
        dir.mkpath(QStringLiteral("."));

    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

QString FrontendManager::get_startup_ui() const {
    return m_startup_ui;
}

void FrontendManager::set_startup_ui(const QString &value) {
    if (m_startup_ui == value)
        return;
    m_startup_ui = value;

    // Write to config file
    m_config_settings.writeEntry("startup_ui", m_startup_ui);
    m_config->sync();

    Q_EMIT startup_ui_changed();
}

bool FrontendManager::get_autostart_enabled() const {
    return m_autostart_enabled;
}

void FrontendManager::set_autostart_enabled(bool value) {
    if (m_autostart_enabled == value)
        return;
    m_autostart_enabled = value;

#ifdef IS_FLATPAK
    // Use libportal to request Flatpak autostart
    g_autoptr(GPtrArray) cmd = nullptr;
    XdpBackgroundFlags flags = value ? XDP_BACKGROUND_FLAG_AUTOSTART : XDP_BACKGROUND_FLAG_NONE;
    xdp_portal_request_background(XdpQt::globalPortalObject(), nullptr, const_cast<char *>("Autostart"), cmd, flags, nullptr, nullptr, this);
#endif

#if !defined(IS_FLATPAK)
    // Get destination path for desktop file (only used for native installation and AppImage)
    const QString destination = QStandardPaths::standardLocations(QStandardPaths::ConfigLocation).value(0) + QStringLiteral("/autostart/de.berny23.virtual_surround_manager.desktop");
#endif

    if (m_autostart_enabled) {
        // Get source path for desktop file (only used for native installation and AppImage)
        // Source can either be /usr/share/applications or ~/.local/share/applications, when installed natively
        QString source = QStringLiteral("/usr/share/applications/de.berny23.virtual_surround_manager.desktop");
        if (!QFile::exists(source))
            source = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation).value(0) + QStringLiteral("/de.berny23.virtual_surround_manager.desktop");

#ifdef IS_APPIMAGE
        // Copy desktop file from extraced AppImage folder that is automatically created under /tmp and randomly named. The APPDIR variable contains the path.
        source = qEnvironmentVariable("APPDIR") + QStringLiteral("/de.berny23.virtual_surround_manager.desktop");
#endif

#if !defined(IS_FLATPAK)
        // Native or AppImage: Just copy the desktop file to the user's autostart folder (e. g. ~/.config/autostart)
        // Creates directory with all parents, mkpath skips existing folders
        QDir().mkpath(QFileInfo(destination).path());
        QFile::remove(destination); // QFile::copy never overwrites
        if (!QFile::copy(source, destination)) {
            // Without the desktop file autostart would silently not work, so
            // roll the setting back instead of persisting a broken state
            qWarning("set_autostart_enabled: Failed to copy desktop file from '%s' to '%s'", source.toStdString().c_str(), destination.toStdString().c_str());
            set_error_message(QStringLiteral("Could not enable autostart: copying the desktop file from '%1' failed.").arg(source));
            m_autostart_enabled = false;
            Q_EMIT autostart_enabled_changed();
            return;
        }
        qDebug("set_autostart_enabled: Copied desktop file from '%s' to '%s'", source.toStdString().c_str(), destination.toStdString().c_str());
#endif
#ifdef IS_APPIMAGE
        // Replace "Exec=virtual-surround-manager" with full path to AppImage
        QFile file(destination);
        if (file.open(QIODevice::ReadWrite | QIODevice::Text)) {
            QString content = QTextStream(&file).readAll();
            content.replace(QStringLiteral("Exec=virtual-surround-manager"), QStringLiteral("Exec=") + qEnvironmentVariable("APPIMAGE"));
            file.seek(0);
            file.resize(0);
            QTextStream(&file) << content;
        }
#endif
    } else {
#if !defined(IS_FLATPAK)
        // Native or AppImage: Remove the desktop file from the user's autostart folder (e. g. ~/.config/autostart)
        QFile::remove(destination);
        qDebug("set_autostart_enabled: Removed desktop file from '%s'", destination.toStdString().c_str());
#endif
    }

    // Write to config file
    m_config_settings.writeEntry("autostart_enabled", m_autostart_enabled);
    m_config->sync();

    Q_EMIT autostart_enabled_changed();
}

bool FrontendManager::get_virtual_surround_auto_enabled() const {
    return m_virtual_surround_auto_enabled;
}

void FrontendManager::set_virtual_surround_auto_enabled(bool value) {
    if (m_virtual_surround_auto_enabled == value)
        return;
    m_virtual_surround_auto_enabled = value;

    // Write to config file
    m_config_settings.writeEntry("virtual_surround_auto_enabled", m_virtual_surround_auto_enabled);
    m_config->sync();

    Q_EMIT virtual_surround_auto_enabled_changed();
}

bool FrontendManager::get_upmix_enabled() const {
    return m_upmix_enabled;
}

void FrontendManager::set_upmix_enabled(bool value) {
    if (m_upmix_enabled == value)
        return;
    m_upmix_enabled = value;

    // The upmix flag is baked into the filter graph, so rebuild the module (this
    // also applies the change to already routed streams). This must happen even
    // while disabled: the module survives a disable, and leaving it stale here
    // would resurrect the old upmix setting on the next enable.
    rebuild_module();

    // Write to config file
    m_config_settings.writeEntry("upmix_enabled", m_upmix_enabled);
    m_config->sync();

    Q_EMIT upmix_enabled_changed();
}
