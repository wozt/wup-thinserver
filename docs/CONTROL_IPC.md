# Local configuration control interface

`wup-thinserver` exposes a local Unix-domain socket so the settings
application can validate, apply, and persist input changes without restarting
the stream or desktop session.

## Endpoint and security

The socket is `${XDG_RUNTIME_DIR}/wup-thinserver/control.sock`. If
`XDG_RUNTIME_DIR` is unavailable, the daemon uses
`/tmp/wup-thinserver-UID/wup-thinserver/control.sock`.

- The containing directory is mode `0700`; the socket is mode `0600`.
- Linux peer credentials must report the same UID as the daemon.
- There is no TCP listener and no remote administration endpoint.
- Requests and replies are one newline-terminated JSON object, limited to
  64 KiB.
- IPC may select only defined safe built-in commands. It cannot introduce an
  arbitrary shell command. Existing advanced commands loaded from the JSON
  file are preserved when unchanged.

The network stream remains unauthenticated and unencrypted. The IPC security
boundary does not change the trusted-LAN requirement of the WUP Stream
protocol.

## Operations

| Operation | Purpose |
|---|---|
| `ping` | Return daemon and current controller state. |
| `get` | Return the current input/audio configuration and state. |
| `validate_input` | Validate a proposed `input` object without changing state. |
| `apply_input` | Validate, atomically save, then publish input settings live. |
| `restore_defaults` | Atomically save and apply the default input settings. |

Every reply contains `ok`. Failed replies contain `error`; successful mutating
replies contain `message`. State contains the connection state and, while a
client is connected, the latest buttons, sticks, touch, volume, and battery
report. It also always contains `input_mode` (the effective server-side mode,
not merely the configured preference), `gamepad_available`, and a human-readable
`input_status`. The `input.mode` member accepted by `validate_input` and
`apply_input` selects Desktop (`1`) or Gamepad (`0`). A Desktop→Gamepad apply
first proves that a uinput device can be created; failure rejects the update
without changing the valid settings file.

## Live update sequence

The daemon parses into a private configuration copy, validates it, and writes
it with a same-directory temporary file plus `fsync` and `rename`. Only after
that succeeds does it publish a complete settings snapshot to the streaming
thread. The streaming thread releases held key chords, modifiers, stick keys,
mouse buttons, virtual gamepad buttons/axes, and touch drags using the old
mapping, clears its edge state, changes input mode when requested, parses the
new mapping, and continues the existing session.

The IPC loop runs in the daemon's main thread. File writes and JSON parsing do
not run in the capture/encode thread.

## Example

```sh
python3 - <<'PY'
import json, os, socket
path = os.path.join(os.environ["XDG_RUNTIME_DIR"],
                    "wup-thinserver", "control.sock")
with socket.socket(socket.AF_UNIX) as sock:
    sock.connect(path)
    sock.sendall(b'{"op":"get"}\n')
    print(json.loads(sock.recv(65536)))
PY
```
