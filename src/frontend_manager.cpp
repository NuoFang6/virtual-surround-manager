#include "frontend_manager.h"
#include "pipewire_manager.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <utility>

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

PipeWireManager::LfeTimbre parse_lfe_timbre(const QString &value) {
    if (value == QStringLiteral("center"))
        return PipeWireManager::LfeTimbre::CenterHrir;
    if (value == QStringLiteral("diffuse"))
        return PipeWireManager::LfeTimbre::Diffuse;
    return PipeWireManager::LfeTimbre::Delta;
}

bool is_valid_lfe_timbre(const QString &value) {
    return value == QStringLiteral("delta") || value == QStringLiteral("center") || value == QStringLiteral("diffuse");
}

} // namespace

FrontendManager::FrontendManager(PipeWireManager *pipewire_manager, KConfig *config, QObject *parent) : QObject(parent) {
    m_pipewire_manager = pipewire_manager;
    m_config = config;
    m_config_settings = m_config->group(QStringLiteral("Settings"));
    connect(m_pipewire_manager, &PipeWireManager::error_occurred, this, &FrontendManager::set_error_message);
    connect(m_pipewire_manager, &PipeWireManager::connection_lost, this, [this]() {
        set_virtual_surround_enabled(false);
        invalidate_hrir_analysis(QStringLiteral("音频服务或效果模块已断开"));
        refresh_input_detection();
    });

    // Initialize UI values. The level-match setting is needed by the first
    // analysis refresh, so read it before loading the preset list.
    m_preset_level_match = m_config_settings.readEntry("preset_level_match", bool());
    m_output_limiter = m_config_settings.readEntry("output_limiter", bool());

    m_lfe_timbre = m_config_settings.readEntry("lfe_timbre", QStringLiteral("diffuse"));
    if (!is_valid_lfe_timbre(m_lfe_timbre)) {
        m_lfe_timbre = QStringLiteral("diffuse");
        m_config_settings.writeEntry("lfe_timbre", m_lfe_timbre);
        m_config->sync();
    }
    m_pipewire_manager->set_lfe_options(PipeWireManager::LfeLowpass::Butterworth4,
                                           parse_lfe_timbre(m_lfe_timbre), true);

    // Read the bass mode/upmix state before the first analysis refresh, so the
    // startup status line never claims "bass off" while the config says otherwise.
    m_upmix_enabled = m_config_settings.readEntry("upmix_enabled", bool());
    m_lfe_mode = m_config_settings.readEntry("lfe_mode", QStringLiteral("balanced"));
    if (!is_valid_lfe_mode(m_lfe_mode)) {
        m_lfe_mode = QStringLiteral("balanced");
        m_config_settings.writeEntry("lfe_mode", m_lfe_mode);
        m_config->sync();
    }
    m_pipewire_manager->set_lfe_mode(parse_lfe_mode(m_lfe_mode));

    load_hrir_wav_files();

    // Remove settings that no longer have any behaviour. `channels` belonged to
    // versions that still supported 5.1 output; `lfe_bypass` was the old toggle
    // for HRIR-colored LFE, which is now always rendered through the LFE timbre IR.
    if (m_config_settings.hasKey("channels") || m_config_settings.hasKey("lfe_bypass") ||
        m_config_settings.hasKey("lfe_lowpass") || m_config_settings.hasKey("lfe_per_ear_alignment")) {
        m_config_settings.deleteEntry("channels");
        m_config_settings.deleteEntry("lfe_bypass");
        m_config_settings.deleteEntry("lfe_lowpass");
        m_config_settings.deleteEntry("lfe_per_ear_alignment");
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
    m_hrir_levels.clear();
    // A disabled graph may contain an older version of a file at the same path.
    // Recreate before enabling, while no application streams are routed to it.
    m_pipewire_manager->remove_virtual_surround_module();
    if (!m_pipewire_manager->create_virtual_surround_module(path.toStdString(), m_upmix_enabled, output_trim_db(),
                                                            m_output_limiter))
        return false;
    if (m_virtual_surround_enabled)
        m_pipewire_manager->enable_routing();
    return true;
}

bool FrontendManager::rebuild_module() {
    // Never leave the previous preset's diagnostic values on screen while the
    // graph is being rebuilt. The final refresh_hrir_analysis() at the end of
    // each setter replaces this sentinel with the new values.
    invalidate_hrir_analysis(QStringLiteral("重建效果图"));
    m_hrir_levels.clear();
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
        if (m_pipewire_manager->replace_virtual_surround_module(path.toStdString(), m_upmix_enabled, output_trim_db(),
                                                               m_output_limiter))
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
    invalidate_hrir_analysis(QStringLiteral("重新加载配置文件"));
    // Check first if a path has already been selected by the user
    const QStringList previous_names = m_hrir_wav_file_names;
    const QStringList previous_paths = m_hrir_wav_file_paths;
    const int previous_index = m_hrir_wav_file_name_index;
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

    m_hrir_levels.clear();

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
    int new_index = paths.indexOf(old_path);
    m_hrir_wav_file_name_index = new_index >= 0 ? new_index : 0;

    // Reload means reload the file too, even when its name/index did not change.
    // Keep the last applied selection visible if the candidate cannot be built.
    if (m_virtual_surround_enabled && !rebuild_module()) {
        m_hrir_wav_file_names = previous_names;
        m_hrir_wav_file_paths = previous_paths;
        m_hrir_wav_file_name_index = previous_index;
    } else {
        m_config_settings.writeEntry("hrir_wav_file_path", m_hrir_wav_file_paths.value(m_hrir_wav_file_name_index));
        m_config->sync();
    }
    Q_EMIT hrir_wav_file_names_changed();
    Q_EMIT hrir_wav_file_name_index_changed();
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
    if (m_autostart_pending || m_autostart_enabled == value) {
        Q_EMIT autostart_enabled_changed();
        return;
    }
#ifdef IS_FLATPAK
    struct Request {
        QPointer<FrontendManager> manager;
        bool enabled;
    };
    m_autostart_pending = true;
    auto *request = new Request{this, value};
    const auto flags = value ? XDP_BACKGROUND_FLAG_AUTOSTART : XDP_BACKGROUND_FLAG_NONE;
    xdp_portal_request_background(XdpQt::globalPortalObject(), nullptr, "Autostart", nullptr,
        flags, nullptr, [](GObject *portal, GAsyncResult *result, gpointer data) {
            auto *request = static_cast<Request *>(data);
            GError *error = nullptr;
            const bool success = xdp_portal_request_background_finish(XDP_PORTAL(portal), result, &error);
            if (request->manager) {
                auto *manager = request->manager.data();
                manager->m_autostart_pending = false;
                if (success) {
                    manager->m_autostart_enabled = request->enabled;
                    manager->m_config_settings.writeEntry("autostart_enabled", request->enabled);
                    manager->m_config->sync();
                } else {
                    manager->set_error_message(QStringLiteral("无法更新自动启动设置：%1")
                        .arg(error ? QString::fromUtf8(error->message) : QStringLiteral("请求被拒绝")));
                }
                Q_EMIT manager->autostart_enabled_changed();
            }
            if (error) g_error_free(error);
            delete request;
        }, request);
    return;
#else
    const QString destination = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
        QStringLiteral("/autostart/de.berny23.virtual_surround_manager.desktop");
    const auto fail = [&](const QString &message) {
        set_error_message(message);
        Q_EMIT autostart_enabled_changed();
    };
    if (value) {
        QString source = QStandardPaths::locate(QStandardPaths::ApplicationsLocation,
            QStringLiteral("de.berny23.virtual_surround_manager.desktop"));
#ifdef IS_APPIMAGE
        source = qEnvironmentVariable("APPDIR") + QStringLiteral("/de.berny23.virtual_surround_manager.desktop");
#endif
        QFile input(source);
        if (!input.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("无法读取自动启动桌面文件：%1").arg(source));
            return;
        }
        QString content = QString::fromUtf8(input.readAll());
#ifdef IS_APPIMAGE
        const QString appimage = qEnvironmentVariable("APPIMAGE");
        if (appimage.isEmpty()) {
            fail(QStringLiteral("无法确定 AppImage 的路径。"));
            return;
        }
        // Exec has its own quoting rules inside desktop-entry string escaping.
        QString argument = QStringLiteral("\"");
        for (const QChar c : appimage) {
            if (c == u'%') argument += QStringLiteral("%%");
            else {
                if (c == u'\\' || c == u'"' || c == u'$' || c == u'`') argument += u'\\';
                argument += c;
            }
        }
        argument += u'"';
        argument.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
        argument.replace(u'\n', QStringLiteral("\\n"));
        argument.replace(u'\r', QStringLiteral("\\r"));
        argument.replace(u'\t', QStringLiteral("\\t"));
        QStringList lines = content.split(u'\n');
        for (QString &line : lines)
            if (line.startsWith(QStringLiteral("Exec="))) line = QStringLiteral("Exec=") + argument;
        content = lines.join(u'\n');
#endif
        QSaveFile output(destination);
        const QByteArray bytes = content.toUtf8();
        if (!QDir().mkpath(QFileInfo(destination).path()) || !output.open(QIODevice::WriteOnly) ||
            output.write(bytes) != bytes.size() || !output.commit()) {
            fail(QStringLiteral("无法写入自动启动桌面文件：%1").arg(destination));
            return;
        }
    } else if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
        fail(QStringLiteral("无法删除自动启动桌面文件：%1").arg(destination));
        return;
    }
    m_autostart_enabled = value;
    m_config_settings.writeEntry("autostart_enabled", value);
    m_config->sync();
    Q_EMIT autostart_enabled_changed();
#endif
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

    const bool applied = !m_virtual_surround_enabled ||
        m_pipewire_manager->update_processing(m_upmix_enabled, parse_lfe_mode(m_lfe_mode));
    if (!applied) {
        m_upmix_enabled = previous_value;
        m_config_settings.writeEntry("upmix_enabled", m_upmix_enabled);
        m_config->sync();
        Q_EMIT upmix_enabled_changed();
    }
    refresh_hrir_analysis();
}

double FrontendManager::hrir_level_for_path(const QString &file_path) {
    const auto cached = m_hrir_levels.constFind(file_path);
    if (cached != m_hrir_levels.cend())
        return cached.value();
    const double level = m_pipewire_manager->hrir_level_db(file_path.toStdString());
    if (std::isfinite(level) && level > -200.0)
        m_hrir_levels.insert(file_path, level);
    return level;
}

double FrontendManager::output_trim_db() {
    if (!m_preset_level_match)
        return 0.0;

    QList<double> levels;
    for (const QString &path : std::as_const(m_hrir_wav_file_paths)) {
        const double level = hrir_level_for_path(path);
        if (level > -200.0)
            levels.append(level);
    }
    if (levels.isEmpty())
        return 0.0;

    std::sort(levels.begin(), levels.end());
    const int count = levels.size();
    const double median = (count % 2 == 1)
                              ? levels.at(count / 2)
                              : 0.5 * (levels.at(count / 2 - 1) + levels.at(count / 2));

    const QString selected_path = m_hrir_wav_file_paths.value(m_hrir_wav_file_name_index);
    if (selected_path.isEmpty())
        return 0.0;
    const double selected = hrir_level_for_path(selected_path);
    if (selected <= -200.0)
        return 0.0;

    return std::clamp(median - selected, -8.0, 8.0);
}

QString FrontendManager::get_hrir_analysis_text() const {
    return m_hrir_analysis_text;
}

void FrontendManager::invalidate_hrir_analysis(const QString &reason) {
    ++m_hrir_analysis_generation;
    const QString text = reason.isEmpty()
                             ? QStringLiteral("HRIR 分析：刷新中…")
                             : QStringLiteral("HRIR 分析：刷新中…（%1）").arg(reason);
    if (text == m_hrir_analysis_text)
        return;
    m_hrir_analysis_text = text;
    qDebug("invalidate_hrir_analysis: %s", text.toStdString().c_str());
    Q_EMIT hrir_analysis_text_changed();
}

void FrontendManager::refresh_hrir_analysis() {
    // Any result computed for an older generation is discarded: a newer
    // invalidation/refresh owns the display now. This also guarantees that a
    // failed/superseded analysis can never leave the previous preset's values
    // on screen.
    const quint64 generation = ++m_hrir_analysis_generation;
    QString text;
    const QString path = m_hrir_wav_file_paths.value(m_hrir_wav_file_name_index);
    if (path.isEmpty()) {
        text = QStringLiteral("HRIR 分析：未获取（未找到有效的 14 声道 HRIR 配置文件）");
    } else {
        const PipeWireManager::HrirAnalysis analysis = m_pipewire_manager->analyze_hrir(path.toStdString());
        if (generation != m_hrir_analysis_generation)
            return;
        if (!analysis.valid) {
            text = QStringLiteral("HRIR 分析不可用（%1：文件无效，或参考响应为零/无法定义相位；不应把缺失分析视为已对齐）")
                       .arg(QFileInfo(path).fileName());
        } else {
            const QString timbre = m_lfe_timbre == QStringLiteral("diffuse")
                ? QStringLiteral("扩散场 HRIR")
                : (m_lfe_timbre == QStringLiteral("center") ? QStringLiteral("中心HRIR") : QStringLiteral("δ"));
            const QString bass = !m_upmix_enabled || m_lfe_mode == QStringLiteral("reference")
                ? QStringLiteral("关闭")
                : (m_lfe_mode == QStringLiteral("impact") ? QStringLiteral("LFE 送量 0 dB（仅上混）")
                                                          : QStringLiteral("LFE 送量 −6 dB（仅上混）"));
            // A valid WAV can still have no usable LFE/main phase reference
            // (e.g. an all-zero or cancelling IR). In that case the analyser
            // returns no per-ear values; showing the default 0.00 would claim
            // an alignment that the graph did not apply.
            const QString main_delay = analysis.per_ear_values_valid
                ? QString::number(analysis.main_delay_ms, 'f', 2) : QStringLiteral("—");
            const QString lfe_delay_l = analysis.per_ear_values_valid
                ? QString::number(analysis.lfe_delay_l_ms, 'f', 2) : QStringLiteral("—");
            const QString lfe_delay_r = analysis.per_ear_values_valid
                ? QString::number(analysis.lfe_delay_r_ms, 'f', 2) : QStringLiteral("—");
            const QString residual_l = analysis.per_ear_values_valid
                ? QString::number(analysis.average_residual_l_degrees, 'f', 1) : QStringLiteral("—");
            const QString residual_r = analysis.per_ear_values_valid
                ? QString::number(analysis.average_residual_r_degrees, 'f', 1) : QStringLiteral("—");
            text = QStringLiteral("%1：%2 Hz；上混FFT 4096 / 128 ms；低频 %3（扩散 LFE）；原生LFE 120Hz BW4 / %4；"
                                  "LFE/主对齐：主 %5 ms、LFE %6/%7 ms、残差 %8/%9°；%10；%11")
                .arg(QFileInfo(path).fileName()).arg(analysis.source_rate).arg(bass).arg(timbre)
                .arg(main_delay).arg(lfe_delay_l).arg(lfe_delay_r).arg(residual_l).arg(residual_r)
                .arg(m_preset_level_match ? QStringLiteral("参考电平匹配 %1 dB").arg(output_trim_db(),0,'f',2) : QStringLiteral("电平匹配关闭"))
                .arg(m_output_limiter ? QStringLiteral("8×重建峰值限幅 / 5 ms") : QStringLiteral("限幅关闭"));
        }
    }

    if (generation != m_hrir_analysis_generation)
        return;
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

    if (m_virtual_surround_enabled &&
        !m_pipewire_manager->update_processing(m_upmix_enabled, parse_lfe_mode(m_lfe_mode))) {
        m_lfe_mode = previous_mode;
        m_pipewire_manager->set_lfe_mode(parse_lfe_mode(m_lfe_mode));
        m_config_settings.writeEntry("lfe_mode", m_lfe_mode);
        m_config->sync();
        Q_EMIT lfe_mode_changed();
    }
    refresh_hrir_analysis();
}

QString FrontendManager::get_lfe_timbre() const {
    return m_lfe_timbre;
}

void FrontendManager::set_lfe_timbre(const QString &value) {
    if (!is_valid_lfe_timbre(value) || m_lfe_timbre == value)
        return;
    const QString previous = m_lfe_timbre;
    m_lfe_timbre = value;
    m_config_settings.writeEntry("lfe_timbre", m_lfe_timbre);
    m_config->sync();
    Q_EMIT lfe_timbre_changed();
    m_pipewire_manager->set_lfe_options(PipeWireManager::LfeLowpass::Butterworth4,
                                           parse_lfe_timbre(m_lfe_timbre), true);
    if (!rebuild_module()) {
        m_lfe_timbre = previous;
        m_config_settings.writeEntry("lfe_timbre", m_lfe_timbre);
        m_config->sync();
        Q_EMIT lfe_timbre_changed();
        m_pipewire_manager->set_lfe_options(PipeWireManager::LfeLowpass::Butterworth4,
                                               parse_lfe_timbre(m_lfe_timbre), true);
    }
    refresh_hrir_analysis();
}

bool FrontendManager::get_preset_level_match() const {
    return m_preset_level_match;
}

void FrontendManager::set_preset_level_match(bool value) {
    if (m_preset_level_match == value)
        return;

    const bool previous_value = m_preset_level_match;
    m_preset_level_match = value;
    m_config_settings.writeEntry("preset_level_match", m_preset_level_match);
    m_config->sync();
    Q_EMIT preset_level_match_changed();

    // The trim is baked into the outGain nodes, so rebuild the effects graph.
    // Keep the previous graph and config if the new one cannot be built.
    if (!rebuild_module()) {
        m_preset_level_match = previous_value;
        m_config_settings.writeEntry("preset_level_match", m_preset_level_match);
        m_config->sync();
        Q_EMIT preset_level_match_changed();
    }
    refresh_hrir_analysis();
}

bool FrontendManager::get_output_limiter() const {
    return m_output_limiter;
}

void FrontendManager::set_output_limiter(bool value) {
    if (m_output_limiter == value)
        return;

    const bool previous_value = m_output_limiter;
    m_output_limiter = value;
    m_config_settings.writeEntry("output_limiter", m_output_limiter);
    m_config->sync();
    Q_EMIT output_limiter_changed();

    if (!rebuild_module()) {
        m_output_limiter = previous_value;
        m_config_settings.writeEntry("output_limiter", m_output_limiter);
        m_config->sync();
        Q_EMIT output_limiter_changed();
    }
    refresh_hrir_analysis();
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
