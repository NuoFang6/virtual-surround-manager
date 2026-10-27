#include "pipewire_manager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QStandardPaths>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string_view>
#include <time.h>

//
// Escapes a path for embedding into a JSON string value of the module
// arguments, so quotes or backslashes in a file name cannot break the config.
//
static string json_escape(const string &value) {
    string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        default:
            escaped += c;
        }
    }
    return escaped;
}

//
// Parses a RIFF/WAVE header far enough to learn the frame count and format of
// the impulse response. Returns false on anything this minimal parser does not
// understand (only plain PCM / IEEE float files are accepted, which covers all
// HeSuvi-style HRIRs).
//
static bool parse_wav_format(const string &path, uint32_t &channels, uint32_t &rate,
                             uint32_t &block_align, uint16_t &bits, uint32_t &frames) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    file.close();

    if (data.size() < 12 || memcmp(data.constData(), "RIFF", 4) != 0 ||
        memcmp(data.constData() + 8, "WAVE", 4) != 0)
        return false;

    bool have_fmt = false;
    uint16_t format_tag = 0;
    size_t offset = 12;
    while (offset + 8 <= static_cast<size_t>(data.size())) {
        const char *id = data.constData() + offset;
        const uint32_t size = qFromLittleEndian<uint32_t>(data.constData() + offset + 4);
        const QByteArray body = data.mid(offset + 8, size);

        if (memcmp(id, "fmt ", 4) == 0 && size >= 16) {
            format_tag = qFromLittleEndian<uint16_t>(body.constData() + 0);
            channels = qFromLittleEndian<uint16_t>(body.constData() + 2);
            rate = qFromLittleEndian<uint32_t>(body.constData() + 4);
            block_align = qFromLittleEndian<uint16_t>(body.constData() + 12);
            bits = qFromLittleEndian<uint16_t>(body.constData() + 14);
            // WAVE_FORMAT_EXTENSIBLE: real bits per sample live in the extension
            if (format_tag == 0xFFFE && size >= 40)
                bits = qFromLittleEndian<uint16_t>(body.constData() + 18);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0 && have_fmt) {
            if (block_align == 0 || channels == 0)
                return false;
            frames = size / block_align;
            // Only plain PCM or float (the formats the convolver accepts anyway)
            return (format_tag == 1 || format_tag == 3) && rate > 0 && frames > 0;
        }
        offset += 8 + size + (size % 2);
    }
    return false;
}

//
// Writes a "dirac delta" impulse response with the exact format and length of
// the selected HRIR: a single full-scale sample at t = 0 followed by silence.
// Convolving the LFE path with this instead of the HRIR leaves the signal
// uncolored (equal L/R, no spatial processing) while keeping the convolver
// branch identical in IR length - and therefore in processing latency - to the
// convolved channels it sums with. This mirrors the conventional design in
// headphone virtualizers where the LFE channel bypasses BRIR convolution and
// is time-aligned with an extra delay.
//
static bool write_lfe_bypass_delta(const string &hrir_path, const string &delta_path) {
    uint32_t channels = 0, rate = 0, block_align = 0, frames = 0;
    uint16_t bits = 0;
    if (!parse_wav_format(hrir_path, channels, rate, block_align, bits, frames))
        return false;

    // Keep a sane cap so a broken header cannot produce a huge file
    if (frames > rate * 2)
        frames = rate * 2;

    // Mono file, same encoding as the source IR
    const uint32_t frame_size = bits / 8;
    const uint32_t data_size = frames * frame_size;

    QFile file(QString::fromStdString(delta_path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    auto le32 = [](uint32_t v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
    auto le16 = [](uint16_t v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };

    QByteArray fmt;
    fmt += le16(3);      // IEEE float
    fmt += le16(1);      // mono
    fmt += le32(rate);
    fmt += le32(rate * frame_size);
    fmt += le16(frame_size);
    fmt += le16(bits);

    QByteArray header;
    header += QByteArray("RIFF", 4);
    header += le32(4 + 8 + 16 + 8 + data_size);
    header += QByteArray("WAVE", 4);
    header += QByteArray("fmt ", 4);
    header += le32(16);
    header += fmt;
    header += QByteArray("data", 4);
    header += le32(data_size);

    QByteArray samples(data_size, '\0');
    if (bits == 32) {
        const float full_scale = 1.0f;
        memcpy(samples.data(), &full_scale, 4);
    } else if (bits == 16) {
        const int16_t full_scale = 32767;
        memcpy(samples.data(), &full_scale, 2);
    } else if (bits == 24) {
        const uint8_t full_scale[3] = {0xFF, 0xFF, 0x7F};
        memcpy(samples.data(), full_scale, 3);
    } else {
        return false;
    }

    const bool ok = file.write(header) == header.size() && file.write(samples) == samples.size();
    file.close();
    return ok;
}

QString PipeWireManager::find_ladspa_plugin() {
    QStringList candidates;

#ifdef IS_APPIMAGE
    // The AppImage is mounted at a random temporary path, resolved via $APPDIR
    const QString appdir = qEnvironmentVariable("APPDIR");
    if (!appdir.isEmpty()) {
        candidates << appdir + QStringLiteral("/usr/lib/ladspa/freesurround_ladspa.so")
                   << appdir + QStringLiteral("/usr/lib64/ladspa/freesurround_ladspa.so");
    }
#endif
#ifndef IS_APPIMAGE
    // Where this application installed the plugin to (absolute path set at build time)
    candidates << QStringLiteral(VSM_LADSPA_PLUGIN_PATH);
#endif

    // Distro or manually installed copies in the usual LADSPA directories
    candidates << QStringLiteral("/usr/lib64/ladspa/freesurround_ladspa.so")
               << QStringLiteral("/usr/lib/ladspa/freesurround_ladspa.so")
               << QStringLiteral("/usr/local/lib/ladspa/freesurround_ladspa.so");

    for (const QString &candidate : candidates) {
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

string PipeWireManager::parse_default_sink_name(const char *value) {
    if (!value)
        return {};
    const string json(value);
    const size_t key = json.find("\"name\"");
    if (key == string::npos)
        return json;
    const size_t colon = json.find(':', key);
    if (colon == string::npos)
        return {};
    const size_t start = json.find('"', colon);
    if (start == string::npos)
        return {};
    const size_t end = json.find('"', start + 1);
    if (end == string::npos)
        return {};
    return json.substr(start + 1, end - start - 1);
}

string PipeWireManager::resolve_sink_name(const string &key) const {
    if (key.empty())
        return {};
    for (auto it = sinks.cbegin(); it != sinks.cend(); ++it)
        if (it.value().node_name == key || it.value().serial == key)
            return it.value().node_name;
    return {};
}

bool PipeWireManager::stream_follows_default(uint32_t id, const string &props_target) const {
    // The metadata target takes precedence over the node's own props target
    string target = metadata_targets.value(id, string());
    if (target.empty())
        target = props_target;

    if (target.empty())
        return true;

    // Streams that still target our virtual sink (any generation) - e.g. routed by a
    // previous instance of this application - keep being managed instead of being
    // treated as assigned elsewhere.
    if (target.rfind(capture_node_name_base, 0) == 0)
        return true;

    const string sink = resolve_sink_name(target);
    if (sink.empty())
        return false;
    return sink == default_sink_name;
}

void PipeWireManager::decide_stream(uint32_t id, const string &props_target) {
    if (!metadata || decided_streams.contains(id))
        return;

    decided_streams.insert(id);

    if (stream_follows_default(id, props_target)) {
        pw_metadata_set_property(metadata, id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", capture_node_name.c_str());
        routed_node_ids.insert(id);
        qInfo("decide_stream: Node %u routed to sink '%s'", id, capture_node_name.c_str());
    } else {
        qInfo("decide_stream: Node %u left alone (assigned to another device)", id);
    }
}

void PipeWireManager::on_node_info(void *data, const struct pw_node_info *info) {
    NodeBinding *binding = static_cast<NodeBinding *>(data);
    PipeWireManager *manager = binding->manager;

    if (!info || !info->props)
        return;

    // Track the state so the application knows whether audio is actually playing
    binding->state = info->state;

    const char *target = spa_dict_lookup(info->props, PW_KEY_TARGET_OBJECT);
    if (!target)
        target = spa_dict_lookup(info->props, "target.node");
    binding->props_target = (target && strlen(target) > 0) ? target : "";
    manager->decide_stream(binding->id, binding->props_target);
    // Keep listening on routed streams: their state changes indicate playback.
    // Detaching is only safe for streams that are not managed by us.
    if (manager->decided_streams.contains(binding->id) && !manager->routed_node_ids.contains(binding->id))
        manager->detach_binding_listener(binding);
    manager->update_playing_state();
}

int PipeWireManager::on_metadata_property(void *data,
                                          uint32_t subject, // Node ID
                                          const char *key,
                                          [[maybe_unused]] const char *type,
                                          const char *value) { // nullptr if property removed
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    if (!key)
        return 0;

    // Track the default sink so we know which streams follow it
    if (strcmp(key, "default.audio.sink") == 0) {
        const string new_default = parse_default_sink_name(value);
        if (new_default != manager->default_sink_name) {
            manager->default_sink_name = new_default;
            qDebug("on_metadata_property: Default sink is now '%s'", new_default.c_str());
        }
        return 0;
    }

    if (strcmp(key, PW_KEY_TARGET_OBJECT) != 0 && strcmp(key, "target.node") != 0)
        return 0;
    if (subject == manager->playback_node_id)
        return 0;

    // Ignore the echo of our own routing writes
    if (value && strcmp(value, manager->capture_node_name.c_str()) == 0)
        return 0;

    // WirePlumber uses "-1" to mean "no explicit target, follow the default sink"
    const bool no_target = !value || strlen(value) == 0 || strcmp(value, "-1") == 0;
    if (no_target)
        manager->metadata_targets.remove(subject);
    else
        manager->metadata_targets[subject] = value;

    if (no_target && manager->routed_node_ids.contains(subject)) {
        // Re-assert routing if a stream we virtualize had its target cleared.
        // A full removal is always countered. Clears to "-1" are normally left alone
        // (re-asserting on them could start a write battle with WirePlumber), but
        // right after a graph replacement they are how WirePlumber drops the stream
        // back onto the physical device - so pin them for a short window, rate limited.
        const qint64 now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        const bool in_pin_window = no_target && value != nullptr && manager->pin_routes_until_ms > now_ms;
        if ((value == nullptr || in_pin_window) && now_ms - manager->last_pin_ms.value(subject, 0) >= 250) {
            manager->last_pin_ms[subject] = now_ms;
            pw_metadata_set_property(manager->metadata, subject, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", manager->capture_node_name.c_str());
            qDebug("on_metadata_property: Pinned node %u to sink '%s'", subject, manager->capture_node_name.c_str());
        }
    } else if (!no_target && manager->routed_node_ids.contains(subject) &&
               strcmp(value, manager->capture_node_name.c_str()) != 0) {
        // The stream was explicitly moved to another device, so it is no longer ours
        manager->routed_node_ids.remove(subject);
        auto binding = manager->node_bindings.find(subject);
        if (binding != manager->node_bindings.end())
            manager->detach_binding_listener(binding.value());
        manager->update_playing_state();
        qDebug("on_metadata_property: Node %u released to target '%s'", subject, value);
    }

    return 0;
}

void PipeWireManager::registry_event_global(void *data,
                                            uint32_t id,
                                            [[maybe_unused]] uint32_t permissions,
                                            const char *type,
                                            [[maybe_unused]] uint32_t version,
                                            const struct spa_dict *props) {
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    // Get the default metadata object
    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *metadata_name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (metadata_name && strcmp(metadata_name, "default") == 0) {
            // Check if there already is a metadata object
            if (manager->metadata) {
                spa_hook_remove(&manager->metadata_listener);
                pw_proxy_destroy((pw_proxy *)manager->metadata);
                manager->metadata = nullptr;
            }

            // State of the previous metadata object does not apply to the new one
            manager->metadata_targets.clear();
            manager->metadata_global_id = id;

            // Save the metadata object for reuse later
            manager->metadata = static_cast<pw_metadata *>(
                pw_registry_bind(manager->registry, id, type, PW_VERSION_METADATA, 0));
            if (!manager->metadata) {
                qWarning("registry_event_global: Failed to bind metadata object with ID %u", id);
                manager->metadata_global_id = SPA_ID_INVALID;
                return;
            }
            // Add listener for changes to node metadata
            spa_zero(manager->metadata_listener);
            pw_metadata_add_listener(manager->metadata, &manager->metadata_listener, &manager->metadata_events, manager);

            // Streams that were tracked before the metadata object appeared have not
            // been decided yet, so decide them now that routing is possible
            for (auto it = manager->node_bindings.cbegin(); it != manager->node_bindings.cend(); ++it) {
                manager->decide_stream(it.key(), it.value()->props_target);
                if (manager->decided_streams.contains(it.key()) && !manager->routed_node_ids.contains(it.key()))
                    manager->detach_binding_listener(it.value());
            }

            // Routing we wrote into a previous metadata object was stored there and
            // is gone with it (e.g. after a WirePlumber restart), so re-assert it
            for (const uint32_t node_id : manager->routed_node_ids) {
                pw_metadata_set_property(manager->metadata, node_id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", manager->capture_node_name.c_str());
                qDebug("registry_event_global: Re-routed node %u to sink '%s' after metadata change", node_id, manager->capture_node_name.c_str());
            }
        }
    }

    // Check if object is a node
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    // Get node name and media class
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *node_media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *node_media_role = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE);

    if (!node_media_class)
        return;

    // Ignore system notification sounds, they are very short and can cause crackling or even a crash
    if (node_name && std::ranges::find(ignored_node_names, std::string_view(node_name)) != ignored_node_names.end())
        return;

    // Track output sinks to resolve stream targets to a device (excluding our own,
    // including sinks of previous graph generations that are still tearing down)
    if (strcmp(node_media_class, "Audio/Sink") == 0 || strcmp(node_media_class, "Audio/Duplex") == 0) {
        if (node_name && strncmp(node_name, manager->capture_node_name_base, strlen(manager->capture_node_name_base)) != 0) {
            const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
            manager->sinks[id] = SinkInfo{node_name, serial ? serial : ""};
        }
        return;
    }

    // Check if object is an audio output stream, but exclude system events and our virtual surround manager source.
    if (strcmp(node_media_class, "Stream/Output/Audio") != 0)
        return;
    if (node_media_role && strcmp(node_media_role, "Notification") == 0)
        return;
    if (node_name && strncmp(node_name, manager->playback_node_name_base, strlen(manager->playback_node_name_base)) == 0) {
        manager->playback_node_id = id;
        return;
    }

    if (manager->node_bindings.contains(id))
        return;

    // Bind the node to read target.object, which the registry global does not include
    pw_proxy *proxy = static_cast<pw_proxy *>(pw_registry_bind(manager->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!proxy) {
        qWarning("registry_event_global: Failed to bind node with ID %u", id);
        return;
    }

    NodeBinding *binding = new NodeBinding{manager, id, proxy, {}, {}};
    spa_zero(binding->listener);
    // Insert before adding the listener: add_listener dispatches the initial node info
    // synchronously and the handler needs to find this binding in the map already.
    manager->node_bindings.insert(id, binding);
    pw_node_add_listener(reinterpret_cast<pw_node *>(binding->proxy), &binding->listener, &manager->node_events, binding);

    qDebug("registry_event_global: Tracking output stream '%s' with ID %u", node_name ? node_name : "(null)", id);
}

void PipeWireManager::registry_event_global_remove(void *data, uint32_t id) {
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    // The bound metadata proxy is only marked removed by the core, not freed,
    // so it is safe to destroy it here ourselves
    if (id == manager->metadata_global_id) {
        if (manager->metadata) {
            spa_hook_remove(&manager->metadata_listener);
            pw_proxy_destroy((pw_proxy *)manager->metadata);
            manager->metadata = nullptr;
        }
        manager->metadata_global_id = SPA_ID_INVALID;
        manager->metadata_targets.clear();
    }

    manager->sinks.remove(id);
    manager->metadata_targets.remove(id);
    manager->routed_node_ids.remove(id);
    manager->decided_streams.remove(id);

    // Do not let a stale ID suppress metadata handling of future nodes reusing it
    if (manager->playback_node_id == id)
        manager->playback_node_id = SPA_ID_INVALID;

    auto binding = manager->node_bindings.find(id);
    if (binding != manager->node_bindings.end()) {
        manager->destroy_binding(binding.value());
        manager->node_bindings.erase(binding);
        manager->update_playing_state();
    }
}

void PipeWireManager::destroy_binding(NodeBinding *binding) {
    if (binding->listener_attached)
        spa_hook_remove(&binding->listener);
    pw_proxy_destroy(binding->proxy);
    delete binding;
}

void PipeWireManager::detach_binding_listener(NodeBinding *binding) {
    if (!binding->listener_attached)
        return;
    spa_hook_remove(&binding->listener);
    binding->listener_attached = false;
}

string PipeWireManager::build_module_args(const string &hrir_wav_path, bool upmix_enabled,
                                          const string &capture_name, const string &playback_name) {
    if (hrir_wav_path.empty()) {
        qWarning("build_module_args: No HRIR file provided");
        return {};
    }

    // The path is interpolated into the JSON module arguments, so escape it
    const string hrir_path = json_escape(hrir_wav_path);

    // Generate the unit impulse IR for the LFE path: it renders LFE without HRIR
    // coloring (equal L/R, no spatial processing) while keeping the convolver
    // branch identical in IR length - and therefore in processing latency - to the
    // convolved channels it sums with. Regenerated per HRIR for that reason.
    const QString delta_file =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/lfe_bypass_delta.wav");
    QDir().mkpath(QFileInfo(delta_file).absolutePath());
    if (!write_lfe_bypass_delta(hrir_wav_path, delta_file.toStdString())) {
        qWarning("build_module_args: Could not create LFE bypass impulse for '%s'", hrir_wav_path.c_str());
        Q_EMIT error_occurred(QStringLiteral("Could not create the LFE bypass impulse response. "
                                             "The selected HRIR file could not be parsed."));
        return {};
    }
    const string lfe_delta_path = json_escape(delta_file.toStdString());
    const string lfe_channel_l = "0";
    const string lfe_channel_r = "0";

    // The FreeSurround upmixer is only part of the graph while upmixing is enabled;
    // without it the graph inputs are the copy nodes directly and the bundled
    // LADSPA plugin is not needed at all.
    string upmix_nodes;
    string upmix_links;
    string graph_inputs = R"(  [ "copyFL:In" "copyFR:In" "copyFC:In" "copyLFE:In" "copyRL:In" "copyRR:In" "copySL:In" "copySR:In" ])";

    if (upmix_enabled) {
        const QString plugin_path = find_ladspa_plugin();
        if (plugin_path.isEmpty()) {
            qWarning("build_module_args: FreeSurround LADSPA plugin not found");
            Q_EMIT error_occurred(QStringLiteral("The FreeSurround upmix plugin (freesurround_ladspa.so) "
                                                 "was not found. Reinstall this application or copy the "
                                                 "plugin into a LADSPA directory like /usr/lib/ladspa."));
            return {};
        }
        qInfo("create_virtual_surround_module: Using FreeSurround plugin '%s'", plugin_path.toStdString().c_str());

        // FreeSurround stereo to 7.1 upmixer with the Real3D-VST auto-detection:
        // stereo content is upmixed, real multichannel content is passed through
        // unchanged (with matching latency compensation). Bass redirection uses a
        // 70-90 Hz transition whose midpoint sits on the THX-standard 80 Hz bass
        // management crossover. The LFE path's own lowpass is separate (120 Hz,
        // Dolby LFE band limit).
        upmix_nodes = R"(
        # FreeSurround stereo to 7.1 upmixer with automatic stereo detection:
        # only upmixes while all non-L/R input channels are silent
        { type = ladspa name = fs plugin = ")" + json_escape(plugin_path.toStdString()) + R"(" label = "freesurround71_auto"
          control = {
              "center_image" = 0.7
              "shift" = 0.0
              "depth" = 1.0
              "circular_wrap" = 90.0
              "focus" = 0.0
              "front_sep" = 1.0
              "rear_sep" = 1.0
              "bass_lo" = 70.0
              "bass_hi" = 90.0
              "use_lfe" = 1.0
              "upmix_enable" = 1.0
          }
        }
)";
        upmix_links = R"(
        # upmixer outputs feed the copy nodes (which fan out to the HRIR convolvers)
        { output = "fs:Out FL"  input="copyFL:In"  }
        { output = "fs:Out FR"  input="copyFR:In"  }
        { output = "fs:Out FC"  input="copyFC:In"  }
        { output = "fs:Out LFE" input="copyLFE:In" }
        { output = "fs:Out RL"  input="copyRL:In"  }
        { output = "fs:Out RR"  input="copyRR:In"  }
        { output = "fs:Out SL"  input="copySL:In"  }
        { output = "fs:Out SR"  input="copySR:In"  }
)";
        graph_inputs = R"(  [ "fs:In FL" "fs:In FR" "fs:In FC" "fs:In LFE" "fs:In RL" "fs:In RR" "fs:In SL" "fs:In SR" ])";
    }

    // LFE gain: native LFE content is recorded 10 dB low (70mm-film convention) and
    // compensated at playback, which HeSuVi's lfc.txt does with 2x gain (EqualizerAPO's
    // "Filter: ON LP Fc 111 Hz" plus 200% LFE). Bass extracted from stereo by the
    // upmixer is at full level, so per the professional monitoring convention (+10 dB
    // applies only to the native LFE channel, not redirected bass) it gets unity gain.
    const string lfe_gain = upmix_enabled ? "1.0" : "2.0";
    // Set 7.1 filter graph for virtual surround module, automatically creating playback and capture nodes
    const string filter_graph = R"(
{
    nodes = [)" + upmix_nodes + R"(
        # duplicate inputs
        { type = builtin label = copy name = copyFL  }
        { type = builtin label = copy name = copyFR  }
        { type = builtin label = copy name = copyFC  }
        { type = builtin label = copy name = copyRL  }
        { type = builtin label = copy name = copyRR  }
        { type = builtin label = copy name = copySL  }
        { type = builtin label = copy name = copySR  }
        { type = builtin label = copy name = copyLFE }

        # apply hrir - HeSuVi 14-channel WAV (not the *-.wav variants) (note: 44 in HeSuVi are the same, but resampled to 44100)
        # Channel order follows HeSuVi's convolution channel line
        # (L0 R1 SL0 SR1 RL0 RR1 C0 R0 L1 SR0 SL1 RR0 RL1 C1 from the stock
        # hesuvi.txt): channels 7-12 interleave the right-ear IRs of the
        # right-side speakers, which is why the indices look shuffled against
        # the speaker labels. Independently verified via the ITD and level
        # asymmetry of the IR files themselves (see "HRIR channel mapping" in
        # README.md) - do not "fix" these based on recordings.
        { type = builtin label = convolver name = convFL_L config = { filename = ")" +
                                    hrir_path + R"(" channel =  0 } }
        { type = builtin label = convolver name = convFL_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  1 } }
        { type = builtin label = convolver name = convSL_L config = { filename = ")" +
                                        hrir_path + R"(" channel =  2 } }
        { type = builtin label = convolver name = convSL_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  3 } }
        { type = builtin label = convolver name = convRL_L config = { filename = ")" +
                                        hrir_path + R"(" channel =  4 } }
        { type = builtin label = convolver name = convRL_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  5 } }
        { type = builtin label = convolver name = convFC_L config = { filename = ")" +
                                        hrir_path + R"(" channel =  6 } }
        { type = builtin label = convolver name = convFR_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  7 } }
        { type = builtin label = convolver name = convFR_L config = { filename = ")" +
                                        hrir_path + R"(" channel =  8 } }
        { type = builtin label = convolver name = convSR_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  9 } }
        { type = builtin label = convolver name = convSR_L config = { filename = ")" +
                                        hrir_path + R"(" channel = 10 } }
        { type = builtin label = convolver name = convRR_R config = { filename = ")" +
                                        hrir_path + R"(" channel = 11 } }
        { type = builtin label = convolver name = convRR_L config = { filename = ")" +
                                        hrir_path + R"(" channel = 12 } }
        { type = builtin label = convolver name = convFC_R config = { filename = ")" +
                                        hrir_path + R"(" channel = 13 } }

        # LFE path: rendered without HRIR convolution (conventional headphone-virtualizer
        # design for the LFE channel). Convolving with a unit impulse of the exact same
        # length as the selected HRIR yields an uncolored, equal-L/R signal while keeping
        # both convolver branches time-aligned (identical IR length -> identical latency)
        { type = builtin label = convolver name = convLFE_L config = { filename = ")" +
                                        lfe_delta_path + R"(" channel = )" +
                                        lfe_channel_l + R"( } }
        { type = builtin label = convolver name = convLFE_R config = { filename = ")" +
                                        lfe_delta_path + R"(" channel = )" +
                                        lfe_channel_r + R"( } }

        # LFE path lowpass: 4th-order Linkwitz-Riley (two cascaded Butterworth biquads,
        # in-phase flat recombination - the professional crossover standard) at 120 Hz,
        # the Dolby LFE band limit (AVR "LPF of LFE" defaults to 48 dB/oct; 24 dB/oct
        # keeps the phase rotation gentler). Then rendered through the center channel
        # HRIRs at a level that depends on the source: 2x for native LFE channels,
        # 1x for bass extracted by the upmixer
        { type = builtin label = bq_lowpass name = lfeLP1 control = { "Freq" = 120.0 "Q" = 0.7071 } }
        { type = builtin label = bq_lowpass name = lfeLP2 control = { "Freq" = 120.0 "Q" = 0.7071 } }
        { type = builtin label = linear name = gainLFE_L control = { "Mult" = )" + lfe_gain + R"( } }
        { type = builtin label = linear name = gainLFE_R control = { "Mult" = )" + lfe_gain + R"( } }

        # stereo output
        { type = builtin label = mixer name = mixL }
        { type = builtin label = mixer name = mixR }

        # Output headroom, matching HeSuVi's level budget: its upmix matrix attenuates the
        # main channels (L=0.5*L, R=0.5*R, ...) so the 8 binaural channels can sum up
        # without clipping. Users compensate with the system volume, applied after here.
        { type = builtin label = linear name = outGainL control = { "Mult" = 0.30 } }
        { type = builtin label = linear name = outGainR control = { "Mult" = 0.30 } }
    ]
    links = [)" + upmix_links + R"(
        # input
        { output = "copyFL:Out"  input="convFL_L:In"  }
        { output = "copyFL:Out"  input="convFL_R:In"  }
        { output = "copySL:Out"  input="convSL_L:In"  }
        { output = "copySL:Out"  input="convSL_R:In"  }
        { output = "copyRL:Out"  input="convRL_L:In"  }
        { output = "copyRL:Out"  input="convRL_R:In"  }
        { output = "copyFC:Out"  input="convFC_L:In"  }
        { output = "copyFR:Out"  input="convFR_R:In"  }
        { output = "copyFR:Out"  input="convFR_L:In"  }
        { output = "copySR:Out"  input="convSR_R:In"  }
        { output = "copySR:Out"  input="convSR_L:In"  }
        { output = "copyRR:Out"  input="convRR_R:In"  }
        { output = "copyRR:Out"  input="convRR_L:In"  }
        { output = "copyFC:Out"  input="convFC_R:In"  }
        { output = "copyLFE:Out" input="lfeLP1:In" }
        { output = "lfeLP1:Out" input="lfeLP2:In" }
        { output = "lfeLP2:Out" input="gainLFE_L:In" }
        { output = "lfeLP2:Out" input="gainLFE_R:In" }
        { output = "gainLFE_L:Out" input="convLFE_L:In" }
        { output = "gainLFE_R:Out" input="convLFE_R:In" }

        # output
        { output = "convFL_L:Out"  input="mixL:In 1" }
        { output = "convFL_R:Out"  input="mixR:In 1" }
        { output = "convSL_L:Out"  input="mixL:In 2" }
        { output = "convSL_R:Out"  input="mixR:In 2" }
        { output = "convRL_L:Out"  input="mixL:In 3" }
        { output = "convRL_R:Out"  input="mixR:In 3" }
        { output = "convFC_L:Out"  input="mixL:In 4" }
        { output = "convFC_R:Out"  input="mixR:In 4" }
        { output = "convFR_R:Out"  input="mixR:In 5" }
        { output = "convFR_L:Out"  input="mixL:In 5" }
        { output = "convSR_R:Out"  input="mixR:In 6" }
        { output = "convSR_L:Out"  input="mixL:In 6" }
        { output = "convRR_R:Out"  input="mixR:In 7" }
        { output = "convRR_L:Out"  input="mixL:In 7" }
        { output = "convLFE_R:Out" input="mixR:In 8" }
        { output = "convLFE_L:Out" input="mixL:In 8" }
        { output = "mixL:Out" input="outGainL:In" }
        { output = "mixR:Out" input="outGainR:In" }
    ]
    inputs  = )" + graph_inputs + R"(
    outputs = [ "outGainL:Out" "outGainR:Out" ]
}
        )";

    // Set capture properties for virtual surround module
    // node.latency keeps the graph quantum at 1024 samples or more while this
    // device is active: without it, a low-latency app can drag the whole chain
    // down to tiny quanta where the 14 convolvers (plus the FFT upmixer) are
    // far more prone to xruns, and every quantum renegotiation is audible.
    // audio.rate pins both ends of the chain to 48000 Hz: the bundled HeSuVi
    // HRIR WAVs are 48000 Hz and while PipeWire's convolver would correctly
    // resample them to any other graph rate (which clock.allowed-rates can
    // switch to at runtime), running them at their native rate avoids that
    // resampling entirely. PipeWire converts between the chain and streams or
    // devices running at other rates.
    const string capture_props = R"(
{
    node.name = )" + capture_name +
                                         R"(
    media.class = "Audio/Sink"
    node.latency = "1024/48000"
    audio.rate = 48000
    audio.channels = 8
    audio.position = [ FL FR FC LFE RL RR SL SR ]
}
        )";

    // Set playback properties for virtual surround module
    const string playback_props = R"(
{
    node.name = )" + playback_name +
                                          R"(
    node.passive = true
    node.latency = "1024/48000"
    audio.rate = 48000
    audio.channels = 2
    audio.position = [ FL FR ]
}
        )";

    // Combine the properites to a single JSON string
    const string args = R"(
{
    node.description = "Virtual Surround Manager"
    media.name = "Virtual Surround Manager"
    filter.graph = )" +
               filter_graph +
               R"(,
    capture.props = )" +
               capture_props + R"(,
    playback.props = )" +
               playback_props + R"(
}
        )";

    return args;
}

bool PipeWireManager::create_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled) {
    if (!is_valid()) {
        Q_EMIT error_occurred(QStringLiteral("Failed connecting to PipeWire audio service."));
        return false;
    }
    if (module) {
        qInfo("create_virtual_surround_module: Virtual surround module already exists");
        return true;
    }

    // Generation 0 keeps the plain names so streams routed by a previous instance
    // of this application are picked up again
    capture_node_name = capture_node_name_base;
    playback_node_name = playback_node_name_base;
    module_generation = 0;

    qInfo("create_virtual_surround_module: Creating module with WAV path '%s', upmixing %s",
          hrir_wav_path.c_str(), upmix_enabled ? "enabled" : "disabled");

    const string args = build_module_args(hrir_wav_path, upmix_enabled, capture_node_name, playback_node_name);
    if (args.empty())
        return false;

    pw_thread_loop_lock(thread_loop);

    module = pw_context_load_module(
        context,
        "libpipewire-module-filter-chain",
        args.c_str(),
        nullptr);
    if (!module) {
        pw_thread_loop_unlock(thread_loop);
        qWarning("create_virtual_surround_module: Failed to load module into PipeWire context");
        Q_EMIT error_occurred(QStringLiteral("Error creating virtual surround device."));
        return false;
    }

    pw_thread_loop_unlock(thread_loop);

    qInfo("create_virtual_surround_module: Successfully created module");

    return true;
}

bool PipeWireManager::replace_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled) {
    if (!module)
        return create_virtual_surround_module(hrir_wav_path, upmix_enabled);

    if (!is_valid()) {
        Q_EMIT error_occurred(QStringLiteral("Failed connecting to PipeWire audio service."));
        return false;
    }

    // Fresh names so the new module can coexist with the running one
    const string new_generation = std::to_string(module_generation + 1);
    const string new_capture_name = string(capture_node_name_base) + "." + new_generation;
    const string new_playback_name = string(playback_node_name_base) + "." + new_generation;

    qInfo("replace_virtual_surround_module: Replacing graph with WAV path '%s', upmixing %s",
          hrir_wav_path.c_str(), upmix_enabled ? "enabled" : "disabled");

    const string args = build_module_args(hrir_wav_path, upmix_enabled, new_capture_name, new_playback_name);
    if (args.empty())
        return false; // Keep the old graph playing with the previous settings

    pw_thread_loop_lock(thread_loop);

    pw_impl_module *new_module = pw_context_load_module(context,
                                                        "libpipewire-module-filter-chain",
                                                        args.c_str(),
                                                        nullptr);
    if (!new_module) {
        pw_thread_loop_unlock(thread_loop);
        qWarning("replace_virtual_surround_module: Failed to load the new module, keeping the old one");
        Q_EMIT error_occurred(QStringLiteral("Error creating virtual surround device."));
        return false;
    }

    // Move every routed stream from the old sink to the new one before the old sink
    // disappears. Streams only ever move between two virtual sinks here - with the
    // old code they briefly fell back to the physical output device (unattenuated,
    // different volume) which caused an audible level jump and made players pause.
    // While the swap settles, pin the target back if WirePlumber clears it ("-1"),
    // which is how it would drop the stream onto the physical device.
    pin_routes_until_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count()
        + 6000;
    if (is_registry_listener_added && metadata) {
        for (const uint32_t id : routed_node_ids) {
            pw_metadata_set_property(metadata, id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", new_capture_name.c_str());
            qDebug("replace_virtual_surround_module: Node %u re-targeted to sink '%s'", id, new_capture_name.c_str());
        }
        // Let the server complete the re-links before the old module is torn down
        sync_with_core(2);
    }

    // Event handlers compare against the member names, so switch them over before
    // the old module's nodes produce their removal events
    capture_node_name = new_capture_name;
    playback_node_name = new_playback_name;
    module_generation += 1;

    pw_impl_module *old_module = module;
    module = new_module;

    // Also sweep up any module whose deferred teardown has not run yet, so that
    // repeated quick switches do not stack up
    destroy_pending_modules_locked();
    sync_with_core(2);

    pw_thread_loop_unlock(thread_loop);

    // The re-target writes above only become effective once WirePlumber has
    // re-linked the streams, which happens asynchronously. Destroying the previous
    // graph right away drops the streams onto the physical device for a moment:
    // audible as a loud level jump (no graph attenuation, different device volume)
    // and enough to make media players pause.
    schedule_module_destroy(old_module);

    qInfo("replace_virtual_surround_module: Successfully replaced module");

    return true;
}

void PipeWireManager::schedule_module_destroy(pw_impl_module *old_module) {
    if (!old_module || !thread_loop)
        return;

    pending_destroy.append(old_module);

    QTimer::singleShot(2000, this, [this, old_module]() {
        // The shutdown path or a following replace may have collected it already
        if (!pending_destroy.removeOne(old_module) || !thread_loop)
            return;

        pw_thread_loop_lock(thread_loop);

        // Safety net: if WirePlumber dropped a stream elsewhere while the graphs were
        // swapped (e.g. onto the physical device, or cleared its target so it follows
        // the default sink again), pull it back to the current graph
        if (metadata) {
            for (const uint32_t id : std::as_const(routed_node_ids)) {
                const string target = metadata_targets.value(id, string());
                if (target != capture_node_name) {
                    qInfo("schedule_module_destroy: Node %u target is '%s', pinning back to graph", id,
                          target.empty() ? "(default)" : target.c_str());
                    pw_metadata_set_property(metadata, id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", capture_node_name.c_str());
                }
            }
        }

        pw_impl_module_destroy(old_module);
        sync_with_core(2);
        pw_thread_loop_unlock(thread_loop);

        qInfo("schedule_module_destroy: Destroyed replaced module");
    });
}

void PipeWireManager::destroy_pending_modules_locked() {
    for (pw_impl_module *m : std::as_const(pending_destroy))
        pw_impl_module_destroy(m);
    pending_destroy.clear();
}

bool PipeWireManager::is_audio_playing() const {
    return audio_playing.load(std::memory_order_relaxed);
}

void PipeWireManager::update_playing_state() {
    bool playing = false;
    for (auto it = node_bindings.cbegin(); it != node_bindings.cend() && !playing; ++it) {
        if (routed_node_ids.contains(it.key()) && it.value()->state == PW_NODE_STATE_RUNNING)
            playing = true;
    }
    audio_playing.store(playing, std::memory_order_relaxed);
}

void PipeWireManager::remove_virtual_surround_module() {
    if (!thread_loop)
        return;

    pw_thread_loop_lock(thread_loop);

    if (module) {
        pw_impl_module_destroy(module);
        module = nullptr;
    }
    // Any graph whose teardown was deferred must also go away now
    destroy_pending_modules_locked();
    // The playback node disappears with the module; do not keep its now stale ID
    playback_node_id = SPA_ID_INVALID;
    audio_playing.store(false, std::memory_order_relaxed);

    // Wait until the server has actually destroyed the module's nodes,
    // recreating it too fast otherwise makes the audio stop completely
    sync_with_core(2);

    pw_thread_loop_unlock(thread_loop);

    qInfo("Removed virtual surround module");
}

void PipeWireManager::enable_routing() {
    // Do nothing if registry listener already exists
    if (is_registry_listener_added)
        return;
    if (!is_valid()) {
        qWarning("enable_routing: Not connected to PipeWire");
        return;
    }

    pw_thread_loop_lock(thread_loop);

    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    spa_zero(registry_listener);
    // Route all new audio output streams to the filter chain, only active while the loop runs
    pw_registry_add_listener(registry, &registry_listener, &registry_events, this);

    is_registry_listener_added = true;

    pw_thread_loop_unlock(thread_loop);

    qInfo("Enabled routing");
}

void PipeWireManager::disable_routing() {
    // Do nothing if registry listener does not exist
    if (!is_registry_listener_added)
        return;
    if (!thread_loop)
        return;

    pw_thread_loop_lock(thread_loop);

    spa_hook_remove(&registry_listener);

    // Iterate over all nodes with our node name set as target.object, then disconnect them from the filter chain
    for (uint32_t id : routed_node_ids) {
        qDebug("disable_routing: clearing target.object for node %u", id);
        if (metadata) {
            // Remove the target object property from the node
            int ret = pw_metadata_set_property(metadata, id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", nullptr);
            qDebug("disable_routing: pw_metadata_set_property returned %d for node %u", ret, id);
        } else {
            qDebug("disable_routing: metadata is nullptr, skipping node %u", id);
        }
    }

    routed_node_ids.clear();

    for (auto it = node_bindings.begin(); it != node_bindings.end(); ++it)
        destroy_binding(it.value());
    node_bindings.clear();
    metadata_targets.clear();
    sinks.clear();
    decided_streams.clear();
    audio_playing.store(false, std::memory_order_relaxed);

    // The registry is re-created on the next enable_routing()
    if (registry) {
        pw_proxy_destroy((pw_proxy *)registry);
        registry = nullptr;
    }

    is_registry_listener_added = false;

    pw_thread_loop_unlock(thread_loop);

    qDebug("Disabled routing");
}

bool PipeWireManager::is_valid() const {
    return thread_loop && context && core;
}

void PipeWireManager::on_core_done(void *data, [[maybe_unused]] uint32_t id, int seq) {
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    if (seq == manager->sync_seq) {
        manager->sync_done = true;
        pw_thread_loop_signal(manager->thread_loop, false);
    }
}

void PipeWireManager::on_core_error(void *data, uint32_t id, [[maybe_unused]] int seq, int res, const char *message) {
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    qWarning("on_core_error: id %u, res %d (%s)", id, res, message ? message : "?");

    // Only surface core-level failures (e.g. a lost connection); errors for
    // individual objects are usually benign races with disappearing nodes
    if (id == PW_ID_CORE && res < 0) {
        // Emitted from the PipeWire thread: the connection to FrontendManager
        // is automatically queued across threads
        Q_EMIT manager->error_occurred(QString::asprintf("PipeWire error: %s", message && message[0] ? message : strerror(-res)));
    }
}

bool PipeWireManager::sync_with_core(int timeout_sec) {
    if (!core || !thread_loop)
        return false;

    sync_done = false;
    sync_seq = pw_core_sync(core, PW_ID_CORE, sync_seq + 1);

    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec += timeout_sec;

    while (!sync_done) {
        if (pw_thread_loop_timed_wait_full(thread_loop, &abstime) != 0) {
            qWarning("sync_with_core: Timed out after %d seconds", timeout_sec);
            return false;
        }
    }
    return true;
}

PipeWireManager::PipeWireManager() {
    // Initialize PipeWire library
    pw_init(nullptr, nullptr);

    // Debug: Output current version of PipeWire library
    qInfo("Compiled with libpipewire %s\n"
          "Linked with libpipewire %s\n",
          pw_get_headers_version(), pw_get_library_version());

    // Errors emitted as a signal here are lost: no slots are connected yet during
    // construction. The frontend queries is_valid() instead and shows its own message.
    const auto fail = [](const char *message) {
        qWarning("%s", message);
    };

    // Create some initial stuff for PipeWire to work
    thread_loop = pw_thread_loop_new("main", nullptr);
    if (!thread_loop) {
        fail("Failed to create PipeWire thread loop");
        return;
    }

    pw_properties *props = pw_properties_new(nullptr, nullptr);
    if (!props) {
        fail("Failed to create PipeWire properties");
        return;
    }
    pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
    pw_properties_set(props, PW_KEY_MEDIA_ROLE, "Music");

    // Required for Flatpak, otherwise it cannot write PipeWire metadata! Also nice to have for native packaging
    pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Manager");

    context = pw_context_new(pw_thread_loop_get_loop(thread_loop), props, 0);
    if (!context) {
        fail("Failed to create PipeWire context");
        return;
    }

    // Always lock before touching loop objects from outside the thread, then unlock!
    pw_thread_loop_lock(thread_loop);

    core = pw_context_connect(context, nullptr, 0);
    if (!core) {
        pw_thread_loop_unlock(thread_loop);
        fail("Failed to connect to PipeWire daemon");
        return;
    }

    // Listen for core roundtrip acknowledgements (see sync_with_core)
    spa_zero(core_listener);
    pw_core_add_listener(core, &core_listener, &core_events, this);

    if (pw_thread_loop_start(thread_loop) < 0) {
        pw_thread_loop_unlock(thread_loop);
        fail("Failed to start the PipeWire thread loop");
        return;
    }

    pw_thread_loop_unlock(thread_loop);
}

PipeWireManager::~PipeWireManager() {
    disable_routing();

    if (thread_loop) {
        pw_thread_loop_lock(thread_loop);

        // Destroy the metadata proxy while holding the lock: its listener can
        // still be dispatched on the PipeWire thread until the loop is stopped
        if (metadata) {
            spa_hook_remove(&metadata_listener);
            pw_proxy_destroy((pw_proxy *)metadata);
            metadata = nullptr;
        }

        // No deferred teardown survives the connection: destroy replaced graphs now
        destroy_pending_modules_locked();

        // Make sure the server has processed the target clears above before the
        // connection goes away
        sync_with_core(2);

        pw_thread_loop_unlock(thread_loop);
    }

    // Clean up PipeWire stuff, tolerating a failed construction
    if (thread_loop)
        pw_thread_loop_stop(thread_loop);
    if (core)
        pw_core_disconnect(core);
    if (context)
        pw_context_destroy(context);
    if (thread_loop)
        pw_thread_loop_destroy(thread_loop);
    pw_deinit();
}
