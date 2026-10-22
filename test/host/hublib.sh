# hublib.sh - start a stub hub and be sure it is OURS.
#
# Sourced by the end-to-end rigs. It exists because of a failure that cost
# an hour: a rig hard-coded a port, something else was already listening on
# it, `nc -z` cheerfully confirmed that *something* answered, and the rig
# then spent its run testing a hub it had not started -- reading an empty
# log and reporting a feature broken. A test that passes or fails for
# reasons outside the thing under test is worse than no test.
#
#   source "$ROOT/test/host/hublib.sh"
#   PORT=$(free_port)
#   start_stub_hub "$PORT" "$RUN/hub.log" --no-fake-share
#   # $HUB_PID is set; the hub has bound and logged that it did

# A port the kernel says is free. Still a race in principle, but a tiny one
# compared with picking a number and hoping.
free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

# start_stub_hub <port> <logfile> [extra stubhub args...]
# Sets HUB_PID. Exits the script if the hub is not ours and listening.
start_stub_hub() {
    local port="$1" log="$2"
    shift 2

    "$ROOT/tools/stubhub.py" --port "$port" "$@" > "$log" 2>&1 &
    HUB_PID=$!

    local i
    for i in $(seq 60); do
        grep -q "stubhub: adc://" "$log" 2>/dev/null && break
        kill -0 "$HUB_PID" 2>/dev/null || break
        sleep 0.25
    done

    if ! kill -0 "$HUB_PID" 2>/dev/null; then
        echo "the stub hub died on port $port -- its output:" >&2
        cat "$log" >&2
        exit 1
    fi
    if ! grep -q "stubhub: adc://" "$log" 2>/dev/null; then
        echo "the stub hub never said it bound port $port:" >&2
        cat "$log" >&2
        kill "$HUB_PID" 2>/dev/null
        exit 1
    fi

    echo "stub hub on $port (pid $HUB_PID)"
}

# await_line <logfile> <extended-regex> [seconds]
#
# Wait for something to have happened, rather than for time to have passed.
# Every rig here drives the UI by piping keystrokes at it, and the keys have
# to arrive after the thing they act on: a list that has been fetched, a hub
# that has answered. Those are all visible in a log somebody is already
# writing, so they can be waited for.
#
# This exists because test-browse fed its first ENTER at a fixed eight
# seconds, which was generous on a developer's machine and not on a two-core
# CI runner, where the key landed before the listing and the rig reported
# the feature broken.
#
# A wait that expires says so and returns non-zero; callers send the key
# anyway, so a marker that changes degrades to the old timing instead of
# hanging.
await_line() {
    local log="$1" pat="$2" limit="${3:-12}" n=0
    while [ "$n" -lt $((limit * 10)) ]; do
        grep -qaE "$pat" "$log" 2>/dev/null && return 0
        sleep 0.1; n=$((n + 1))
    done
    echo "TIMED OUT after ${limit}s waiting in $(basename "$log"): $pat" >&2
    return 1
}
