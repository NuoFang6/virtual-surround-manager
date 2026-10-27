#pragma once

#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <array>
#include <atomic>
#include <pipewire/extensions/metadata.h>
#include <pipewire/impl-module.h>
#include <pipewire/pipewire.h>
#include <string>
#include <string_view>

using std::string;

class PipeWireManager : public QObject {
    Q_OBJECT

  public:
    PipeWireManager();
    ~PipeWireManager() override;

    //
    // True if the connection to the PipeWire audio service was established.
    // While this is false, the routing methods are no-ops and creating the
    // virtual surround module fails with an error message.
    //
    bool is_valid() const;

    //
    // Creates a new 7.1 virtual surround module. Does nothing if a module already exists.
    // Call this before enable_routing().
    // When upmix_enabled is set, stereo streams are upmixed to 7.1 by the bundled
    // FreeSurround LADSPA plugin (see find_ladspa_plugin()).
    // The LFE path is always rendered without HRIR coloring, through a unit impulse
    // ("dirac delta") IR of the same length as the selected HRIR, matching the
    // conventional headphone-virtualizer design for the LFE channel.
    //
    bool create_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled);

    //
    // Replaces the graph of the running module without interrupting routed streams:
    // the new module is created first (under a fresh node name), all streams that were
    // routed to the old sink are re-targeted to the new one (virtual sink to virtual
    // sink - they never touch the physical device in between), then the old module is
    // destroyed. Falls back to create_virtual_surround_module() when no module exists.
    //
    bool replace_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled);

    //
    // True while at least one of the streams routed to the virtual sink is running
    // (i.e. audio is actually playing through this application).
    //
    bool is_audio_playing() const;

    //
    // Removes the virtual surround module.
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

    Q_SIGNAL void error_occurred(const QString &message);

  private:
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
    // Graph rebuilds get fresh names ("<base>.<generation>") so the old and new
    // module can coexist while routed streams are moved between them. Generation 0
    // keeps the plain base names so streams routed by a previous app instance
    // (whose target.object was persisted in the metadata) are picked up again.
    string capture_node_name = capture_node_name_base;
    string playback_node_name = playback_node_name_base;
    uint32_t module_generation = 0;
    // Thanks to EasyEffects for the list
    constexpr static auto ignored_node_names = std::to_array<std::string_view>({"libcanberra", "org.gnome.VolumeControl", "GNOME Shell", "Mutter", "gsd-media-keys"});

    bool is_registry_listener_added = false;
    QSet<uint32_t> routed_node_ids;
    QSet<uint32_t> decided_streams;
    uint32_t playback_node_id = SPA_ID_INVALID;
    pw_impl_module *module = nullptr;

    // Written from the PipeWire thread, read from the UI thread
    std::atomic<bool> audio_playing{false};

    string default_sink_name;

    QMap<uint32_t, string> metadata_targets;

    struct SinkInfo {
        string node_name;
        string serial;
    };
    QMap<uint32_t, SinkInfo> sinks;

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
    // Builds the libpipewire-module-filter-chain JSON arguments for the given
    // names. Returns an empty string (after emitting an error) if the LFE bypass
    // impulse for the HRIR could not be generated.
    //
    string build_module_args(const string &hrir_wav_path, bool upmix_enabled,
                             const string &capture_name, const string &playback_name);

    //
    // Returns true if a stream has no explicit target or targets the default sink.
    //
    bool stream_follows_default(uint32_t id, const string &props_target) const;

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
};
