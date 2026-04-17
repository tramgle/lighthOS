# SYS_TTY_WINSZ round-trip: read default (24x80), write 42x137, read
# back. `test_winsize` restores 24x80 on exit so later scripts see
# the same defaults.
/bin/test_winsize > /scratch/ws
grep "default 24x80" /scratch/ws | wc -l > /scratch/ws_default
assert winsize.default 1 /scratch/ws_default

grep "after-set 42x137" /scratch/ws | wc -l > /scratch/ws_set
assert winsize.set 1 /scratch/ws_set
