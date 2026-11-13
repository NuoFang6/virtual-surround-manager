#pragma once

#include <QMap>
#include <QMutex>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <pipewire/extensions/metadata.h>
#include <pipewire/impl-module.h>
#include <pipewire/pipewire.h>
#include <string>
#include <string_view>

#include "output_equalizer.h"
#include "../third_party/freesurround_ladspa/dsp_config.h"

using std::string;

class PipeWireManager : public QObject {
    Q_OBJECT

  public:
    PipeWireManager();
    ~PipeWireManager() override;

    //
    // LFE enhancement around the selected timbre (see build_effects_module_args).
    //
    enum class LfeMode {
        Balanced,  // native LFE + a -6 dB mono bass send while upmixing
        Reference, // native LFE only, no enhancement
        Impact,    // the same send at 0 dB (falls back to -6 dB when unaligned)
    };
    void set_lfe_mode(LfeMode mode);
    bool update_processing(bool upmix_enabled, LfeMode mode);
    void write_controls_locked(std::initializer_list<std::pair<const char *, float>> values);

    // The UI uses Butterworth4 and per-ear alignment. Older shapes remain in
    // the internal analysis API for comparisons, not as user-facing settings.
    enum class LfeLowpass {
        Lr4,          // current: two cascaded Q=0.7071 low-passes, -6 dB at 120
        Butterworth4, // flat passband, -3 dB at 120, 24 dB/oct
        Butterworth8, // flat passband, -3 dB at 120, 48 dB/oct (AVR-like)
    };
    enum class LfeTimbre {
        Delta,      // current: uncolored normalized impulse
        CenterHrir, // center HRIR pair, colored center-like LFE
        Diffuse,    // coherent average of the seven HRIR pairs (legacy config name)
    };
    void set_lfe_options(LfeLowpass lowpass, LfeTimbre timbre, bool per_ear_alignment);

    //
    // Diagnostic result for one HRIR: the delta-IR normalization gain and the
    // per-preset LFE/main alignment delay. Used by the UI so imported presets
    // show exactly what the graph will do with them.
    //
    struct HrirAnalysis {
        bool valid = false;
        double reference_gain = 0.0;
        double reference_gain_db = 0.0;
        double alignment_ms = 0.0;
        bool delay_main = false;
        bool delay_lfe = false;
        double residual_degrees = 0.0;        // linear slope-fit residual
        double average_residual_degrees = 0.0; // legacy field name: wrapped phase RMS after delay
        double worst_channel_residual_degrees = 0.0;
        bool allpass_valid = false;
        double allpass_freq = 0.0;
        double allpass_q = 0.0;
        double allpass_residual_degrees = 0.0;
        double allpass_worst_channel_degrees = 0.0;
        // Source WAV sample rate. The graph is always 48 kHz; a different rate
        // is resampled at load time (quality 14), and the alignment above is
        // already computed for the 48 kHz graph.
        uint32_t source_rate = 0;
        bool resampled = false;
        // A-weighted output power for equal, uncorrelated pink-noise inputs;
        // a static reference level, not programme loudness or LUFS.
        double level_db = 0.0;
        // Per-ear LFE alignment. In this mode the LFE path gets separate L/R
        // delays and all-pass sections while the main path uses one common
        // delay. per_ear_values_valid is false when the analyser returned no
        // L/R values at all (the UI must show "—", not 0.00).
        bool per_ear_alignment = false;
        bool per_ear_values_valid = false;
        LfeTimbre timbre = LfeTimbre::Diffuse;
        double main_delay_ms = 0.0;
        double lfe_delay_l_ms = 0.0;
        double lfe_delay_r_ms = 0.0;
        double average_residual_l_degrees = 0.0;
        double average_residual_r_degrees = 0.0;
        double worst_channel_residual_l_degrees = 0.0;
        double worst_channel_residual_r_degrees = 0.0;
        bool allpass_l_valid = false;
        double allpass_l_freq = 0.0;
        double allpass_l_q = 0.0;
        double allpass_l_residual_degrees = 0.0;
        double allpass_l_worst_channel_degrees = 0.0;
        bool allpass_r_valid = false;
        double allpass_r_freq = 0.0;
        double allpass_r_q = 0.0;
        double allpass_r_residual_degrees = 0.0;
        double allpass_r_worst_channel_degrees = 0.0;
    };
    HrirAnalysis analyze_hrir(const string &path) const;

    //
    // Relative A-weighted reference level for seven uncorrelated equal-power
    // pink-noise inputs, integrated below Nyquist. Used only for preset trim.
    //
    double hrir_level_db(const string &path) const;

    //
    // The auto upmixer's current decision: 1 = stereo (upmixing), 0 = multichannel
    // (bypassing), -1 = unknown (plugin not loaded or no audio processed yet).
    //
    int upmix_auto_state() const;

    //
    // True while streams are intentionally left unvirtualized because the
    // physical output is a built-in speaker.
    //
    bool is_speaker_bypass() const;

    //
    // Monotonic count of processed graph blocks. Changes only while audio is
    // actually flowing through the effects graph.
    //
    uint64_t upmix_block_count() const;

    struct LimiterStatistics {
        bool available = false;
        double current_reduction_db = 0.0;
        double maximum_reduction_db = 0.0;
        double reduced_percent = 0.0;
        double active_seconds = 0.0;
        uint64_t active_samples = 0;
        uint64_t reduced_samples = 0;
        bool input_peak_valid = false;
        double input_peak = 0.0;
        double threshold = 0.0;
    };
    LimiterStatistics limiter_statistics() const;
    void set_limiter_statistics_enabled(bool enabled);

    //
    // True if the connection to the PipeWire audio service was established.
    // While this is false, the routing methods are no-ops and creating the
    // virtual surround module fails with an error message.
    //
    bool is_valid() const;

    //
    // Creates the virtual surround device: a permanent bridge sink that applications
    // are routed to, plus the effects graph (upmixer and HRIR convolvers) behind it.
    // Does nothing if the device already exists. Call this before enable_routing().
    // When upmix_enabled is set, stereo streams are upmixed to 7.1 by the bundled
    // FreeSurround LADSPA plugin (see find_ladspa_plugin()).
    // Delta/diffuse intermediates are float32, at the source rate and length.
    // Center timbre uses the center HRIR pair. Only delta is uncolored.
    //
    bool create_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled,
                                        double output_trim_db = 0.0,
                                        const OutputEqConfig &output_eq = {});

    //
    // Replaces only the effects graph behind the permanent bridge sink. Applications
    // keep their stream linked to the same sink, so nothing falls back to the physical
    // device and WirePlumber's MPRIS pause-on-sink-removal never triggers. The new
    // effects graph is created first, the bridge is re-linked to it, and the previous
    // graph is destroyed only after the new link is observed.
    // Falls back to create_virtual_surround_module() when no device exists.
    //
    bool replace_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled,
                                         double output_trim_db = 0.0,
                                         const OutputEqConfig &output_eq = {});

    //
    // True while at least one of the streams routed to the virtual sink is running
    // (i.e. audio is actually playing through this application).
    //
    bool is_audio_playing() const;

    //
    // Removes the bridge and effects modules. Call disable_routing() first: this
    // waits briefly for the routed streams to leave the bridge sink, because
    // removing a sink while streams are still linked to it makes WirePlumber pause
    // their MPRIS players.
    //
    void remove_virtual_surround_module();

    //
    // Starts the PipeWire loop, enabling routing of audio data through the virtual surround node.
    //
    void enable_routing();

    //
    // Stops the PipeWire loop, disabling routing of audio data through the virtual surround node.
    //
    void disable_routing();

    QString physical_output_name() const;
    QString physical_output_description() const;

    Q_SIGNAL void error_occurred(const QString &message);
    Q_SIGNAL void connection_lost();
    Q_SIGNAL void physical_output_changed(const QString &node_name, const QString &description);

  private:
    std::atomic<bool> connected{false};
    std::atomic<bool> shutting_down{false};
    bool loop_started = false;
    bool ensure_connection();
    bool routing_stopping = false;
    struct ModuleBinding {
        PipeWireManager *manager;
        pw_impl_module *module;
        spa_hook listener{};
        uint64_t generation;
        bool expected_destroy = false;
    };
    QHash<pw_impl_module *, ModuleBinding *> module_bindings;
    uint64_t module_generation = 0;
    void track_module_locked(pw_impl_module *module);
    void destroy_module_locked(pw_impl_module *module);
    static void on_module_destroy(void *data);
    const pw_impl_module_events module_events = {
        .version = PW_VERSION_IMPL_MODULE_EVENTS,
        .destroy = on_module_destroy,
    };
    struct pw_thread_loop *thread_loop = nullptr;
    struct pw_context *context = nullptr;
    struct pw_core *core = nullptr;
    struct pw_registry *registry = nullptr;
    struct pw_metadata *metadata = nullptr;
    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    struct spa_hook metadata_listener;

    // State for sync_with_core()
    int sync_seq = 0;
    bool sync_done = false;

    // Global ID of the bound "default" metadata object, SPA_ID_INVALID while unbound
    uint32_t metadata_global_id = SPA_ID_INVALID;

    const char *capture_node_name_base = "effect_input.virtual-surround-manager";
    const char *playback_node_name_base = "effect_output.virtual-surround-manager";

    // The app-facing bridge sink keeps the plain base name forever. Applications are
    // routed to it once and never moved again, not even while the effects graph is
    // replaced. Only the effects sink behind it gets a per-generation name.
    string capture_node_name = capture_node_name_base;
    string effects_capture_node_name;
    string effects_playback_node_name;
    uint32_t effects_generation = 0;

    LfeMode lfe_mode = LfeMode::Balanced;
    // The former experiments are now the defaults: Butterworth 4th-order LFE
    // low-pass, diffuse-field LFE IR and per-ear LFE alignment.
    LfeLowpass lfe_lowpass = LfeLowpass::Butterworth4;
    LfeTimbre lfe_timbre = LfeTimbre::Diffuse;
    bool lfe_per_ear_alignment = true;
    // Gated mono main-sum send levels fixed at graph build time by the HRIR's
    // post-alignment phase residual (0 when the send is unsafe). Runtime LFE
    // mode changes must not exceed what the current graph was fitted for.
    double lfe_send_balanced_ = VsmDsp::balanced_lfe_send;
    double lfe_send_impact_ = VsmDsp::balanced_lfe_send;

    // Handle and symbol of the LADSPA plugin's exported auto-detection state.
    // The filter-chain module loads the plugin in this process, so dlopen()
    // returns the already loaded object and dlsym() reads the same atomic the
    // running graph writes. Holding the handle keeps the symbol valid across
    // effects graph replacements.
    void *upmix_state_handle = nullptr;
    std::atomic<int> *upmix_state = nullptr;
    std::atomic<uint64_t> *upmix_blocks = nullptr;
    std::atomic<uint32_t> *limiter_current_gain_bits = nullptr;
    std::atomic<uint32_t> *limiter_min_gain_bits = nullptr;
    std::atomic<uint64_t> *limiter_active_samples = nullptr;
    std::atomic<uint64_t> *limiter_reduced_samples = nullptr;
    std::atomic<uint64_t> *limiter_latest_generation = nullptr;
    std::atomic<uint64_t> *limiter_active_generation = nullptr;
    std::atomic<uint32_t> *limiter_interval_peak_bits = nullptr;
    std::atomic<bool> *limiter_statistics_enabled = nullptr;
    std::atomic<bool> limiter_statistics_requested{false};
    void reset_limiter_statistics(uint64_t generation);

    // Thanks to EasyEffects for the list
    constexpr static auto ignored_node_names = std::to_array<std::string_view>({"libcanberra", "org.gnome.VolumeControl", "GNOME Shell", "Mutter", "gsd-media-keys"});

    bool is_registry_listener_added = false;
    QSet<uint32_t> routed_node_ids;
    QSet<uint32_t> decided_streams;
    pw_impl_module *bridge_module = nullptr;
    pw_impl_module *effects_module = nullptr;

    // Proxy for the effects playback node (the 2ch stream feeding the physical
    // device). It lets us gate the post-graph output so a new track cannot
    // leak its first samples or the previous track's delay-line residue.
    pw_proxy *effects_playback_control_proxy = nullptr;
    uint32_t effects_playback_control_id = SPA_ID_INVALID;
    bool replacing_effects = false;
    void bind_effects_control_locked();
    void clear_effects_control_locked();
    std::atomic<bool> graph_upmix_enabled{false};
    std::atomic<uint64_t> output_gate_generation{0};

    // Replaced effects graphs stay alive for a short settle period after the bridge
    // has moved: WirePlumber updates its own link bookkeeping asynchronously, and a
    // sink removed while it still considers a link present would pause MPRIS players
    QList<pw_impl_module *> pending_effects_destroy;

    // Node IDs of the bridge/effects nodes, learned from the registry. The bridge
    // playback node is the internal 8-channel stream that feeds the effects sink.
    uint32_t bridge_capture_id = SPA_ID_INVALID;
    uint32_t bridge_playback_id = SPA_ID_INVALID;
    uint32_t effects_capture_id = SPA_ID_INVALID;
    uint32_t effects_playback_id = SPA_ID_INVALID;
    QSet<uint32_t> internal_playback_ids;
    QSet<uint32_t> internal_capture_ids;

    // Live PipeWire links (node id pairs), used to wait until the bridge really
    // feeds the new effects sink before the old graph is destroyed.
    struct LinkInfo {
        uint32_t output_node;
        uint32_t input_node;
    };
    QHash<uint32_t, LinkInfo> links;
    std::atomic<bool> link_wait_active{false};

    // Written from the PipeWire thread, read from the UI thread
    std::atomic<bool> audio_playing{false};

    string default_sink_name;

    QMap<uint32_t, string> metadata_targets;

    struct SinkInfo {
        string node_name;
        string serial;
        string form_factor;
        string description;
        int priority = 0;
        bool mute_known = false;
        bool muted = false;
    };
    QMap<uint32_t, SinkInfo> sinks;
    mutable QMutex physical_output_mutex_;
    string physical_output_name_;
    string physical_output_description_;

    // Last default sink that was not one of ours; used to give the user's real
    // output device back when WirePlumber selects a virtual sink.
    string last_physical_default_sink;

    // True while the effective physical output is a built-in device (speakers).
    // Streams following the default are then left on the physical device.
    std::atomic<bool> speaker_bypass{false};

    bool is_our_sink_name(const string &name) const;
    bool is_effects_sink_name(const string &name) const;
    string choose_physical_default_sink() const;
    uint32_t physical_output_id() const;
    bool physical_default_is_internal() const;
    bool restore_physical_default();
    void update_speaker_bypass();
    void update_physical_output();
    void enforce_virtual_sink_unmuted();
    static void write_node_mute(pw_proxy *proxy, bool mute);
    void release_routed_streams();
    void route_default_streams();
    struct NodeBinding {
        PipeWireManager *manager;
        uint32_t id;
        struct pw_proxy *proxy;
        struct spa_hook listener;
        // Last seen target.object of the node, kept so streams that were tracked
        // before the metadata object appeared can still be decided later
        string props_target;
        // False once the listener was detached after the stream was decided
        bool listener_attached = true;
        // Last seen node state, used to detect whether routed streams are playing
        enum pw_node_state state = PW_NODE_STATE_CREATING;
    };
    QMap<uint32_t, NodeBinding *> node_bindings;

    struct SinkBinding {
        PipeWireManager *manager;
        uint32_t id;
        struct pw_proxy *proxy;
        struct spa_hook listener;
        bool internal = false;
        bool mute_known = false;
        bool muted = false;
    };
    QMap<uint32_t, SinkBinding *> sink_bindings;

    // Throttle for redirecting a stream that was manually moved to the internal
    // effects sink; WirePlumber can emit several link events during the move.
    QHash<uint32_t, qint64> effects_redirect_ms;

    void attach_binding_listener(NodeBinding *binding);

    //
    // Returns the path of the FreeSurround LADSPA plugin used for stereo upmixing,
    // or an empty string if it was not found anywhere.
    //
    static QString find_ladspa_plugin();

    //
    // Blocks until the PipeWire server has processed everything sent so far,
    // or until the timeout (in seconds) expires. Must be called with the thread
    // loop lock held. Replaces blind sleeps after module teardown: audio just
    // stopping when destroying too fast is really just missing synchronization.
    //
    bool sync_with_core(int timeout_sec);

    //
    // Called when the server acknowledges a sync_with_core() roundtrip.
    //
    static void on_core_done(void *data, uint32_t id, int seq);

    //
    // Called on core-level errors (e.g. a lost connection to the PipeWire
    // service). Without this, such failures went completely unnoticed.
    //
    static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message);

    const struct pw_core_events core_events = {
        .version = PW_VERSION_CORE_EVENTS,
        .done = on_core_done,
        .error = on_core_error,
    };

    //
    // Parses the node name out of the default.audio.sink metadata value, e.g. {"name":"alsa_output..."}.
    //
    static string parse_default_sink_name(const char *value);

    //
    // Removes a node binding's listener and destroys its proxy.
    //
    void destroy_binding(NodeBinding *binding);

    //
    // Detaches a still-attached node binding's listener. Called once the stream
    // was decided: no further node events are needed, but the proxy itself is
    // only destroyed on global_remove/disable_routing because destroying a
    // proxy from inside its own event callback is not safe.
    //
    void detach_binding_listener(NodeBinding *binding);

    //
    // Resolves a target (node name or object serial) to a known sink's node name.
    //
    string resolve_sink_name(const string &key) const;

    //
    // Recomputes audio_playing from the states of all routed stream nodes.
    // Must be called from the PipeWire thread.
    //
    void update_playing_state();

    //
    // Temporarily gates the post-graph signal on a newly routed stream. The
    // application owns these gain controls; user mute/volume are never changed.
    //
    void fade_in_new_stream(uint32_t id);
    void write_output_gate_locked(float gain);
    bool gate_effects_output_locked();

    //
    // Schedules destruction of a replaced effects graph. Callers must not hold the
    // thread loop lock.
    //
    void schedule_effects_destroy(pw_impl_module *old_module, uint32_t old_sink_id, int attempts_left = 20);

    //
    // Destroys all deferred effects modules. Must be called with the thread loop
    // lock held.
    //
    void destroy_pending_effects_locked();

    //
    // Builds the effects graph JSON arguments. Returns an empty string (after
    // emitting an error) if the LFE bypass impulse for the HRIR could not be
    // generated.
    //
    string build_effects_module_args(const string &hrir_wav_path, bool upmix_enabled,
                                     const string &capture_name, const string &playback_name,
                                     double output_trim_db,
                                     const OutputEqConfig &output_eq);

    //
    // Builds the permanent bridge graph: an 8-channel sink that fans out to an
    // 8-channel playback stream targeting the effects sink.
    //
    string build_bridge_module_args(const string &effects_sink_name);

    //
    // Loads the effects module under fresh node names, recording the new sink and
    // playback node IDs as they appear in the registry. Must be called with the
    // thread loop lock held. Returns nullptr (and restores the previous names) on
    // failure.
    //
    pw_impl_module *load_effects_module_locked(const string &hrir_wav_path, bool upmix_enabled,
                                               double output_trim_db,
                                               const OutputEqConfig &output_eq);

    //
    // Loads the bridge module if it does not exist yet. Must be called with the
    // thread loop lock held.
    //
    bool load_bridge_module_locked();

    //
    // Waits (releasing the thread loop lock) until the predicate returns true or
    // timeout_ms expires. The predicate runs with the lock held. Returns whether
    // the predicate became true.
    //
    bool wait_for(const std::function<bool()> &predicate, int timeout_ms);

    //
    // True if a link from output_node to input_node is present. Lock must be held.
    //
    bool has_link(uint32_t output_node, uint32_t input_node) const;
    bool has_outgoing_link(uint32_t output_node) const;
    bool has_input_link(uint32_t input_node) const;

    //
    // Returns true if a stream has no explicit target or targets the default sink.
    //
    bool stream_follows_default(uint32_t id, const string &props_target) const;
    bool stream_targets_ours(uint32_t id, const string &props_target) const;

    //
    // Decides once whether to virtualize a stream, then routes it if so.
    // Deciding only once avoids fighting with apps that toggle their own target repeatedly.
    //
    void decide_stream(uint32_t id, const string &props_target);

    //
    // Called when a metadata property of a node changes.
    // This is required for compatibility with EasyEffects etc.
    //
    static int on_metadata_property(void *data,
                                    uint32_t subject, // Node ID
                                    const char *key,
                                    [[maybe_unused]] const char *type,
                                    const char *value);

    const struct pw_metadata_events metadata_events = {
        .version = PW_VERSION_METADATA_EVENTS,
        .property = on_metadata_property,
    };

    //
    // Called when a new object (node) is created in the PipeWire registry.
    //
    static void registry_event_global(void *data,
                                      uint32_t id,
                                      [[maybe_unused]] uint32_t permissions,
                                      const char *type,
                                      [[maybe_unused]] uint32_t version,
                                      const struct spa_dict *props);

    static void registry_event_global_remove(void *data, uint32_t id);

    const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = registry_event_global,
        .global_remove = registry_event_global_remove,
    };

    //
    // Called with a bound output node's info; reads target.object from its full props.
    //
    static void on_node_info(void *data, const struct pw_node_info *info);

    const struct pw_node_events node_events = {
        .version = PW_VERSION_NODE_EVENTS,
        .info = on_node_info,
        .param = nullptr,
    };

    //
    // Called with a bound sink node's info; reads device.form-factor from the
    // full props (the registry global does not carry it).
    //
    static void on_sink_info(void *data, const struct pw_node_info *info);
    static void on_sink_param(void *data, int seq, uint32_t id, uint32_t index,
                              uint32_t next, const struct spa_pod *param);

    const struct pw_node_events sink_node_events = {
        .version = PW_VERSION_NODE_EVENTS,
        .info = on_sink_info,
        .param = on_sink_param,
    };
};
