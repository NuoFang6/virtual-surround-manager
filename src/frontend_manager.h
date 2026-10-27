#pragma once

#include <KConfig>
#include <KConfigGroup>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class PipeWireManager;

class FrontendManager : public QObject {
    Q_OBJECT
    // Whether virtual surround sound is enabled or not
    Q_PROPERTY(bool virtualSurroundEnabled READ get_virtual_surround_enabled WRITE set_virtual_surround_enabled NOTIFY virtual_surround_enabled_changed)
    // The error message to show in the UI
    Q_PROPERTY(QString errorMessage READ get_error_message WRITE set_error_message NOTIFY error_message_changed)
    // The list of HRIR WAV file names found in the directory
    Q_PROPERTY(QStringList hrirWavFileNames READ get_hrir_wav_file_names NOTIFY hrir_wav_file_names_changed)
    // The user-selected index of the HRIR WAV file list
    Q_PROPERTY(int hrirWavFileNameIndex READ get_hrir_wav_file_name_index WRITE set_hrir_wav_file_name_index NOTIFY hrir_wav_file_name_index_changed)

    // The selected startup setting
    Q_PROPERTY(QString startupUi READ get_startup_ui WRITE set_startup_ui NOTIFY startup_ui_changed)
    // Whether autostart is enabled or not
    Q_PROPERTY(bool autostartEnabled READ get_autostart_enabled WRITE set_autostart_enabled NOTIFY autostart_enabled_changed)
    // Whether virtual surround sound is enabled automatically or not
    Q_PROPERTY(bool virtualSurroundAutoEnabled READ get_virtual_surround_auto_enabled WRITE set_virtual_surround_auto_enabled NOTIFY virtual_surround_auto_enabled_changed)
    // Whether stereo audio is upmixed to surround (PSD) or not
    Q_PROPERTY(bool upmixEnabled READ get_upmix_enabled WRITE set_upmix_enabled NOTIFY upmix_enabled_changed)

    // Set while a profile/upmix switch waits for the user to accept the risk of a
    // briefly louder output, because the playing streams could not be muted first
    Q_PROPERTY(bool switchNeedsConfirm READ get_switch_needs_confirm NOTIFY switch_needs_confirm_changed)

  public:
    explicit FrontendManager(PipeWireManager *pipewire_manager, KConfig *config, QObject *parent = nullptr);

    bool get_virtual_surround_enabled() const;
    void set_virtual_surround_enabled(bool value);
    Q_SIGNAL void virtual_surround_enabled_changed();

    QString get_error_message() const;
    void set_error_message(const QString &value);
    Q_SIGNAL void error_message_changed();

    QStringList get_hrir_wav_file_names() const;
    Q_SIGNAL void hrir_wav_file_names_changed();

    int get_hrir_wav_file_name_index() const;
    void set_hrir_wav_file_name_index(int index);
    Q_SIGNAL void hrir_wav_file_name_index_changed();

    // Load HRIR WAV files from data directory
    Q_INVOKABLE void load_hrir_wav_files();

    // Open folder in default file explorer
    Q_INVOKABLE void open_hrir_wav_folder();

    QString get_startup_ui() const;
    void set_startup_ui(const QString &value);
    Q_SIGNAL void startup_ui_changed();

    bool get_autostart_enabled() const;
    void set_autostart_enabled(bool value);
    Q_SIGNAL void autostart_enabled_changed();

    bool get_virtual_surround_auto_enabled() const;
    void set_virtual_surround_auto_enabled(bool value);
    Q_SIGNAL void virtual_surround_auto_enabled_changed();

    bool get_upmix_enabled() const;
    void set_upmix_enabled(bool value);
    Q_SIGNAL void upmix_enabled_changed();

    // True while audio is actually playing through the virtual surround sink
    Q_INVOKABLE bool is_audio_playing() const;

    bool get_switch_needs_confirm() const;
    Q_SIGNAL void switch_needs_confirm_changed();

    // Answers the confirmation request announced by switchNeedsConfirm
    Q_INVOKABLE void resolve_switch_confirm(bool proceed);

  private:
    bool m_virtual_surround_enabled = false;
    QString m_error_message;
    QStringList m_hrir_wav_file_names;
    QStringList m_hrir_wav_file_paths;
    int m_hrir_wav_file_name_index = 0;
    QString m_startup_ui;
    bool m_autostart_enabled = false;
    bool m_virtual_surround_auto_enabled = false;
    bool m_upmix_enabled = false;
    bool m_switch_needs_confirm = false;
    std::function<void()> m_pending_switch_apply;
    std::function<void()> m_pending_switch_cancel;

    PipeWireManager *m_pipewire_manager;
    KConfig *m_config;
    KConfigGroup m_config_settings;

    QString hrir_wav_subpath = QStringLiteral("/hrir_wav");

    bool does_hrir_wav_file_exist(const QString &file_path);

    //
    // Creates the virtual surround module from the current settings and starts
    // routing if it is enabled. Returns false (with an error message set) if the
    // module could not be created.
    //
    bool create_module();

    //
    // Rebuilds the virtual surround module with the current settings, releasing
    // and re-routing all streams.
    //
    void rebuild_module();

    //
    // Runs a change that needs the filter graph rebuilt. While audio plays, the
    // routed streams are muted first - the mute follows them onto whatever sink they
    // end up linked to - then applied, then undone once the swap has settled. When
    // they cannot all be muted, a loud fallback cannot be excluded, so the setting
    // change is held back and the UI asks the user instead; cancel() then takes the
    // already-announced setting back.
    //
    void run_switch(std::function<void()> apply, std::function<void()> cancel);

    //
    // Applies a rolled-back setting to the state and the config file only. Used when
    // the user declined a switch: the running graph was never touched, so rebuilding
    // it (and asking again) would be wrong.
    //
    void rollback_upmix_enabled(bool value);
    void rollback_hrir_selection(int index);
};
