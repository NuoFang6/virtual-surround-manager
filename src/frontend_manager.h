#pragma once

#include <KConfig>
#include <KConfigGroup>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

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
    // Low-frequency handling mode: "balanced", "reference" or "impact"
    Q_PROPERTY(QString lfeMode READ get_lfe_mode WRITE set_lfe_mode NOTIFY lfe_mode_changed)
    // LFE IR/timbre: "diffuse" (default), "delta" or "center". The LFE low-pass
    // (Butterworth 4th) and per-ear alignment are fixed defaults.
    Q_PROPERTY(QString lfeTimbre READ get_lfe_timbre WRITE set_lfe_timbre NOTIFY lfe_timbre_changed)
    // Optional: normalize the overall gain of each HRIR preset to the median
    // A-weighted level of the current preset list
    Q_PROPERTY(bool presetLevelMatch READ get_preset_level_match WRITE set_preset_level_match NOTIFY preset_level_match_changed)
    // Optional: 5 ms lookahead output safety limiter (-1.5 dBFS)
    Q_PROPERTY(bool outputLimiter READ get_output_limiter WRITE set_output_limiter NOTIFY output_limiter_changed)
    // Live input detection text for the main page (refreshed while it is visible)
    Q_PROPERTY(QString inputDetectionText READ get_input_detection_text NOTIFY input_detection_text_changed)
    // Diagnostic line for the selected HRIR (normalization gain and alignment delay)
    Q_PROPERTY(QString hrirAnalysisText READ get_hrir_analysis_text NOTIFY hrir_analysis_text_changed)

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

    QString get_lfe_mode() const;
    void set_lfe_mode(const QString &value);
    Q_SIGNAL void lfe_mode_changed();

    QString get_lfe_timbre() const;
    void set_lfe_timbre(const QString &value);
    Q_SIGNAL void lfe_timbre_changed();

    bool get_preset_level_match() const;
    void set_preset_level_match(bool value);
    Q_SIGNAL void preset_level_match_changed();

    bool get_output_limiter() const;
    void set_output_limiter(bool value);
    Q_SIGNAL void output_limiter_changed();

    QString get_input_detection_text() const;
    Q_SIGNAL void input_detection_text_changed();

    QString get_hrir_analysis_text() const;
    Q_SIGNAL void hrir_analysis_text_changed();

    // Recomputes the input detection text from the current graph state
    Q_INVOKABLE void refresh_input_detection();

    // True while audio is actually playing through the virtual surround sink
    Q_INVOKABLE bool is_audio_playing() const;

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
    QString m_lfe_mode = QStringLiteral("balanced");
    QString m_lfe_timbre = QStringLiteral("diffuse");
    bool m_preset_level_match = false;
    bool m_output_limiter = false;
    QHash<QString, double> m_hrir_levels;
    QString m_input_detection_text;
    QString m_hrir_analysis_text = QStringLiteral("HRIR 分析：尚未获取");
    quint64 m_hrir_analysis_generation = 0;
    uint64_t m_last_auto_blocks = 0;
    qint64 m_last_audio_ms = 0;

    PipeWireManager *m_pipewire_manager;
    KConfig *m_config;
    KConfigGroup m_config_settings;

    QString hrir_wav_subpath = QStringLiteral("/hrir_wav");

    bool does_hrir_wav_file_exist(const QString &file_path);

    //
    // Cached relative A-weighted level of one HRIR (see hrir_level_db).
    //
    double hrir_level_for_path(const QString &file_path);

    //
    // Overall output trim in dB. 0 when preset loudness matching is off;
    // otherwise the selected preset is moved to the median level of all
    // readable 14-channel presets (bounded to +/-8 dB).
    //
    double output_trim_db();

    //
    // Replaces the diagnostic line with an explicit "refreshing" sentinel, so
    // the UI never keeps showing values from a previous preset/settings while
    // the graph is rebuilt or the analysis is unavailable.
    //
    void invalidate_hrir_analysis(const QString &reason);

    //
    // Recomputes the diagnostic line for the currently selected HRIR.
    //
    void refresh_hrir_analysis();

    //
    // Creates the virtual surround module from the current settings and starts
    // routing if it is enabled. Returns false (with an error message set) if the
    // module could not be created.
    //
    bool create_module();

    //
    // Rebuilds the virtual surround device with the current settings. While routing
    // is enabled only the effects graph behind the permanent bridge sink is
    // replaced, so playing streams are not moved. Returns false (with the previous
    // graph still running) when the new graph could not be built.
    //
    bool rebuild_module();
};
