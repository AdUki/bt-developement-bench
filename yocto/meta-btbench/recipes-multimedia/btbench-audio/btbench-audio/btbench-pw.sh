# Root's shell on the board talks to the system-wide PipeWire (meta-btbench, btbench-audio).
# pw-cli, pw-dump, pw-top, pw-cat and wpctl look for the daemon's socket in $PIPEWIRE_RUNTIME_DIR,
# and without it in $XDG_RUNTIME_DIR, root's own runtime dir, where no PipeWire runs.
export PIPEWIRE_RUNTIME_DIR=/run/pipewire
