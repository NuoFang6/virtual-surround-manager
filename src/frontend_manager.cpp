#include "frontend_manager.h"
#include "pipewire_manager.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#ifdef IS_FLATPAK
#include <libportal-qt6/portal-qt6.h>
#include <libportal/background.h>
#endif

namespace {

PipeWireManager::LfeMode parse_lfe_mode(const QString &value) {
    if (value == QStringLiteral("reference"))
        return PipeWireManager::LfeMode::Reference;
    if (value == QStringLiteral("impact"))
        return PipeWireManager::LfeMode::Impact;
    return PipeWireManager::LfeMode::Balanced;
}

bool is_valid_lfe_mode(const QString &value) {
    return value == QStringLiteral("balanced") || value == QStringLiteral("reference") || value == QStringLiteral("impact");
}

} // namespace

FrontendManager::FrontendManager(PipeWireManager *pipewire_manager, KConfig *config, QObject *parent) : QObject(parent) {
    m_pipewire_manager = pipewire_manager;
    m_config = config;
    m_config_settings = m_config->group(QStringLiteral("Settings"));
    connect(m_pipewire_manager, &PipeWireManager::error_occurred, this, &FrontendManager::set_error_message);

    // Initialize UI values

    load_hrir_wav_files();
    refresh_hrir_analysis();

    // Remove settings that no longer have any behaviour. `channels` belonged to
    // versions that still supported 5.1 output; `lfe_bypass` was the old toggle
    // for HRIR-colored LFE, which is now always bypassed through the delta IR.
    if (m_config_settings.hasKey("channels") || m_config_settings.hasKey("lfe_bypass")) {
        m_config_settings.deleteEntry("channels");
        m_config_settings.deleteEntry("lfe_bypass");
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

    m_lfe_mode = m_config_settings.readEntry("lfe_mode", QStringLiteral("balanced"));
    if (!is_valid_lfe_mode(m_lfe_mode)) {
        m_lfe_mode = QStringLiteral("balanced");
        m_config_settings.writeEntry("lfe_mode", m_lfe_mode);
        m_config->sync();
    }
    m_pipewire_manager->set_lfe_mode(parse_lfe_mode(m_lfe_mode));

    m_virtual_surround_auto_enabled = m_config_settings.readEntry("virtual_surround_auto_enabled", bool());
    if (m_virtual_surround_auto_enabled) {
        // This line gets intentionally skipped if auto-enabling is disabled
        set_virtual_surround_enabled(true);
    }

    // Show why enabling virtual surround sound is impossible on a broken PipeWire connection
    if (!m_pipewire_manager->is_valid()) {
        set_error_message(QStringLiteral("连接 PipeWire 音频服务失败。"));
    }

    // Headless measurement hook: Wayland sessions cannot be automated by the test
    // tooling, so this cycles profiles/upmix through the same setters the UI uses.
    // Inert unless VSM_TEST_AUTO_SWITCH_MS is set to a positive interval.
    const int test_switch_ms = qEnvironmentVariableIntValue("VSM_TEST_AUTO_SWITCH_MS");
    if (test_switch_ms > 0) {
        auto *timer = new QTimer(this);
        timer->setInterval(test_switch_ms);
        connect(timer, &QTimer::timeout, this, [this]() {
            static int step = 0;
            ++step;
            qInfo("VSM_TEST switch step %d at %lld", step, static_cast<long long>(QDateTime::currentMSecsSinceEpoch()));
            if (step % 2 == 1) {
                if (m_hrir_wav_file_names.size() > 1) {
                    const int next = (m_hrir_wav_file_name_index + 1) % m_hrir_wav_file_names.size();
                    set_hrir_wav_file_name_index(next);
                }
            } else {
                set_upmix_enabled(!m_upmix_enabled);
            }
        });
        timer->start();
        qInfo("VSM_TEST_AUTO_SWITCH_MS=%d: cycling profiles/upmix for measurements", test_switch_ms);
    }
}

bool FrontendManager::does_hrir_wav_file_exist(const QString &file_path) {
    if (!QFile(file_path).exists()) {
        qWarning("does_hrir_wav_file_exist: HRIR file does not exist: %s", file_path.toStdString().c_str());
        set_error_message(QStringLiteral("所选 HRIR 文件不存在。"));
        return false;
    }
    return true;
}

bool FrontendManager::create_module() {
    // A fresh attempt invalidates any error message from a previous failure
    set_error_message(QString());

    if (m_hrir_wav_file_names.isEmpty()) {
        qWarning("create_module: No virtualization profiles available");
        set_error_message(QStringLiteral("未找到虚拟化配置文件。请把 HRIR WAV 文件放入下方文件夹，然后点击“重新加载”。"));
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

bool FrontendManager::rebuild_module() {
    // A fresh attempt invalidates any error message from a previous failure
    set_error_message(QString());

    if (m_hrir_wav_file_names.isEmpty()) {
        qWarning("rebuild_module: No virtualization profiles available");
        set_error_message(QStringLiteral("未找到虚拟化配置文件。请把 HRIR WAV 文件放入下方文件夹，然后点击“重新加载”。"));
        return false;
    }

    QString path = m_hrir_wav_file_paths.value(m_hrir_wav_file_name_index);
    if (!does_hrir_wav_file_exist(path))
        return false;

    if (m_virtual_surround_enabled) {
        // Only the effects graph is replaced. The sink applications are routed to
        // stays alive, so their streams are never moved: no fallback to the physical
        // device (loud) and no sink removal for WirePlumber to pause players over.
        if (m_pipewire_manager->replace_virtual_surround_module(path.toStdString(), m_upmix_enabled))
            return true;

        // Do not tear the device down on failure: that is exactly what causes the
        // level jump and the player pause. Keep the previous graph running.
        qWarning("rebuild_module: Effects graph replacement failed, keeping the previous graph");
        return false;
    }

    // Routing is off and nothing is linked to the bridge, so a full rebuild is safe
    m_pipewire_manager->remove_virtual_surround_module();
    return create_module();
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

    const int previous_index = m_hrir_wav_file_name_index;
    m_hrir_wav_file_name_index = index;
    Q_EMIT hrir_wav_file_name_index_changed();

    // Write to config file
    m_config_settings.writeEntry("hrir_wav_file_path", m_hrir_wav_file_paths.value(index));
    m_config->sync();

    if (!rebuild_module()) {
        // The previous graph is still running; keep the UI and config in sync with it
        m_hrir_wav_file_name_index = previous_index;
        m_config_settings.writeEntry("hrir_wav_file_path", m_hrir_wav_file_paths.value(previous_index));
        m_config->sync();
        Q_EMIT hrir_wav_file_name_index_changed();
    }

    refresh_hrir_analysis();
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

    refresh_hrir_analysis();
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
            set_error_message(QStringLiteral("无法启用自动启动：复制桌面文件失败（%1）。").arg(source));
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

    const bool previous_value = m_upmix_enabled;
    m_upmix_enabled = value;
    Q_EMIT upmix_enabled_changed();

    // Write to config file
    m_config_settings.writeEntry("upmix_enabled", m_upmix_enabled);
    m_config->sync();

    // The upmix flag is baked into the filter graph, so rebuild the module (this
    // also applies the change to already routed streams). This must happen even
    // while disabled: the module survives a disable, and leaving it stale here
    // would resurrect the old upmix setting on the next enable.
    if (!rebuild_module()) {
        m_upmix_enabled = previous_value;
        m_config_settings.writeEntry("upmix_enabled", m_upmix_enabled);
        m_config->sync();
        Q_EMIT upmix_enabled_changed();
    }
}

QString FrontendManager::get_hrir_analysis_text() const {
    return m_hrir_analysis_text;
}

void FrontendManager::refresh_hrir_analysis() {
    QString text;
    const QString path = m_hrir_wav_file_paths.value(m_hrir_wav_file_name_index);
    if (!path.isEmpty()) {
        const PipeWireManager::HrirAnalysis analysis = m_pipewire_manager->analyze_hrir(path.toStdString());
        if (!analysis.valid) {
            text = QStringLiteral("当前 HRIR 分析：无法读取（需要有效的 14 声道 HeSuVi WAV）");
        } else {
            QString alignment;
            if (analysis.delay_main)
                alignment = QStringLiteral("主声道延迟 %1 ms").arg(analysis.alignment_ms, 0, 'f', 2);
            else if (analysis.delay_lfe)
                alignment = QStringLiteral("LFE 延迟 %1 ms").arg(analysis.alignment_ms, 0, 'f', 2);
            else if (analysis.residual_degrees > 15.0)
                alignment = QStringLiteral("相位非线性，未插入对齐");
            else
                alignment = QStringLiteral("无需额外对齐");

            const QString correction = analysis.allpass_valid
                                           ? QStringLiteral("全通 %1 Hz/Q%2（校正后平均 %3°、最坏 %4°）")
                                                 .arg(analysis.allpass_freq, 0, 'f', 0)
                                                 .arg(analysis.allpass_q, 0, 'f', 2)
                                                 .arg(analysis.allpass_residual_degrees, 0, 'f', 1)
                                                 .arg(analysis.allpass_worst_channel_degrees, 0, 'f', 1)
                                           : QStringLiteral("未启用全通校正");

            text = QStringLiteral("当前 HRIR：LFE 归一化 %1 dB；低频对齐：%2；拟合残差 %3°，对齐后平均 %4°，最坏声道 %5°；相位校正：%6")
                       .arg(analysis.reference_gain_db, 0, 'f', 2)
                       .arg(alignment)
                       .arg(analysis.residual_degrees, 0, 'f', 1)
                       .arg(analysis.average_residual_degrees, 0, 'f', 1)
                       .arg(analysis.worst_channel_residual_degrees, 0, 'f', 1)
                       .arg(correction);
        }
    }

    if (text == m_hrir_analysis_text)
        return;
    m_hrir_analysis_text = text;
    qDebug("refresh_hrir_analysis: %s", text.toStdString().c_str());
    Q_EMIT hrir_analysis_text_changed();
}

bool FrontendManager::is_audio_playing() const {
    return m_pipewire_manager->is_audio_playing();
}

QString FrontendManager::get_lfe_mode() const {
    return m_lfe_mode;
}

void FrontendManager::set_lfe_mode(const QString &value) {
    if (!is_valid_lfe_mode(value) || m_lfe_mode == value)
        return;

    const QString previous_mode = m_lfe_mode;
    m_lfe_mode = value;
    Q_EMIT lfe_mode_changed();

    m_config_settings.writeEntry("lfe_mode", m_lfe_mode);
    m_config->sync();
    m_pipewire_manager->set_lfe_mode(parse_lfe_mode(m_lfe_mode));

    // Like the upmix switch, the mode is baked into the filter graph. While
    // routing is enabled only the effects graph is replaced behind the stable
    // bridge sink, so a switch during playback keeps the stream in place.
    if (!rebuild_module()) {
        m_lfe_mode = previous_mode;
        m_pipewire_manager->set_lfe_mode(parse_lfe_mode(m_lfe_mode));
        m_config_settings.writeEntry("lfe_mode", m_lfe_mode);
        m_config->sync();
        Q_EMIT lfe_mode_changed();
    }
}

QString FrontendManager::get_input_detection_text() const {
    return m_input_detection_text;
}

void FrontendManager::refresh_input_detection() {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

    // The PipeWire node state alone proved unreliable for "is audio playing"
    // (streams can be RUNNING while their state listener was detached). The
    // plugin's process counter only advances while real blocks flow through the
    // effects graph, so use that as the heartbeat instead.
    if (!m_virtual_surround_enabled || !m_upmix_enabled) {
        // Remember the current count without treating it as new audio
        m_last_auto_blocks = m_pipewire_manager->upmix_block_count();
        m_last_audio_ms = 0;
    } else {
        const uint64_t blocks = m_pipewire_manager->upmix_block_count();
        if (blocks != m_last_auto_blocks) {
            m_last_auto_blocks = blocks;
            m_last_audio_ms = now_ms;
        }
    }
    const bool audio_flowing = m_last_audio_ms > 0 && now_ms - m_last_audio_ms < 750;

    QString text;
    if (!m_virtual_surround_enabled) {
        text = QStringLiteral("输入检测：虚拟环绕声已关闭");
    } else if (m_pipewire_manager->is_speaker_bypass()) {
        text = QStringLiteral("输入检测：输出为内置扬声器，已自动跳过处理");
    } else if (!m_upmix_enabled) {
        text = QStringLiteral("输入检测：上混已关闭（直通）");
    } else if (!audio_flowing) {
        text = QStringLiteral("输入检测：等待音频播放…");
    } else {
        switch (m_pipewire_manager->upmix_auto_state()) {
        case 1:
            text = QStringLiteral("输入检测：检测到立体声，上混已生效");
            break;
        case 0:
            text = QStringLiteral("输入检测：检测到多声道，自动直通");
            break;
        default:
            text = QStringLiteral("输入检测：正在检测输入…");
            break;
        }
    }

    if (text == m_input_detection_text)
        return;
    m_input_detection_text = text;
    qDebug("refresh_input_detection: %s", text.toStdString().c_str());
    Q_EMIT input_detection_text_changed();
}
