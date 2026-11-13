#!/usr/bin/env bash
set -euo pipefail

app=${1:?application path required}
hrir=${2:?HRIR path required}

root=$(mktemp -d "${TMPDIR:-/tmp}/vsm-mute-test-XXXXXX")
run="$root/run"
config="$root/config"
home="$root/home"
mkdir -m 700 -p "$run" "$config/wireplumber/wireplumber.conf.d" "$home/.local/share/virtual-surround-manager/hrir_wav"
cp "$hrir" "$home/.local/share/virtual-surround-manager/hrir_wav/test.wav"
printf '%s\n' 'wireplumber.profiles = { vsm-test = { inherits = [ policy, mixin.stateless ] } }' > "$config/wireplumber/wireplumber.conf.d/90-vsm-test.conf"
printf '[Settings]\nhrir_wav_file_path=%s/.local/share/virtual-surround-manager/hrir_wav/test.wav\nstartup_ui=hideTray\nvirtual_surround_auto_enabled=true\nupmix_enabled=true\n' "$home" > "$config/virtual-surround-manager"

export XDG_RUNTIME_DIR="$run"
export PIPEWIRE_RUNTIME_DIR="$run"
export XDG_CONFIG_HOME="$config"
export XDG_DATA_HOME="$home/.local/share"
export HOME="$home"
export QT_QPA_PLATFORM=offscreen
export QT_LOGGING_TO_CONSOLE=1
export QT_FORCE_STDERR_LOGGING=1

pw_pid=
wp_pid=
app_pid=
cleanup() {
    kill "${app_pid:-}" "${wp_pid:-}" "${pw_pid:-}" 2>/dev/null || true
    wait "${app_pid:-}" "${wp_pid:-}" "${pw_pid:-}" 2>/dev/null || true
    rm -rf "$root"
}
trap cleanup EXIT

fail() {
    printf '[FAIL] %s\n' "$1" >&2
    printf '%s\n' '--- application log ---' >&2
    grep -E 'virtual|mute|error|Error|failed|Failed' "$root/app.log" >&2 || true
    exit 1
}

wait_until() {
    local timeout=$1
    shift
    local end=$((SECONDS + timeout))
    while (( SECONDS < end )); do
        if "$@"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_metadata() {
    pw-metadata -n default 2>/dev/null | grep -q Found
}

is_muted() {
    wpctl get-volume "$1" 2>/dev/null | grep -q '\[MUTED\]'
}

is_unmuted() {
    ! is_muted "$1"
}

pipewire -c pipewire.conf > "$root/pipewire.log" 2>&1 &
pw_pid=$!
wait_until 5 test -S "$run/pipewire-0" || fail 'private PipeWire did not start'

wireplumber -p vsm-test > "$root/wireplumber.log" 2>&1 &
wp_pid=$!
wait_until 5 wait_for_metadata || fail 'private WirePlumber did not start'

pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = vsm_test_physical node.description = "VSM Test Physical" media.class = Audio/Sink object.linger = true audio.position = [ FL FR ] priority.session = 1000000 }' > "$root/create.log" 2>&1
physical=
find_physical() {
    physical=$(pw-dump 2>/dev/null | jq -r '.[] | select(.type == "PipeWire:Interface:Node") | select(.info.props["node.name"] == "vsm_test_physical") | .id' | head -n1)
    [[ -n "$physical" ]]
}
wait_until 5 find_physical || fail 'test physical sink did not appear'

"$app" > "$root/app.log" 2>&1 &
app_pid=$!
virtual_ids=
find_virtual_sinks() {
    virtual_ids=$(pw-dump 2>/dev/null | jq -r '[.[] | select(.type == "PipeWire:Interface:Node") | select(.info.props["media.class"] == "Audio/Sink") | select(.info.props["node.name"] | startswith("effect_input.virtual-surround-manager")) | .id] | join(" ")')
    [[ $(wc -w <<< "$virtual_ids") -eq 2 ]]
}
wait_until 10 find_virtual_sinks || fail 'virtual sinks did not appear'

restore_disabled=$(pw-dump 2>/dev/null | jq '[.[] | select(.type == "PipeWire:Interface:Node") | select(.info.props["node.name"] | strings | startswith("effect_input.virtual-surround-manager")) | select(.info.props["state.restore-props"] == false)] | length')
[[ $restore_disabled -eq 2 ]] || fail 'virtual sinks still allow persisted mute restoration'

wpctl set-mute "$physical" 1
for id in $virtual_ids; do
    wpctl set-mute "$id" 1
done
sleep 0.5
is_muted "$physical" || fail 'physical sink was not muted'
for id in $virtual_ids; do
    is_muted "$id" || fail 'virtual sink ignored physical mute'
done

wpctl set-mute "$physical" 0
for id in $virtual_ids; do
    wait_until 3 is_unmuted "$id" || fail 'virtual sink stayed muted after physical output was unmuted'
done

for id in $virtual_ids; do
    wpctl set-mute "$id" 1
    wait_until 3 is_unmuted "$id" || fail 'virtual sink accepted mute while physical output was unmuted'
done

corrections=$(grep -c 'enforce_virtual_sink_unmuted: cleared mute' "$root/app.log" || true)
[[ $corrections -ge 2 ]] || fail 'application did not report both virtual mute corrections'
printf '[PASS] virtual sinks follow the physical output mute boundary\n'
