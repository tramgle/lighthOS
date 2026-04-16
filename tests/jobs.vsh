# Job control smoke test. `sleep 0.3 &` backgrounds; `jobs` lists it as
# Running while it's still alive; `fg` blocks until completion; then
# `jobs` prints nothing.

sleep 0.3 &
jobs > /scratch/jblist
cat /scratch/jblist | grep Running | wc -l > /scratch/jbcount
assert jobs.running 1 /scratch/jbcount

# Foreground and wait; fg emits a line echoing the command.
fg > /scratch/fgout

# Job is reaped after fg. Next `jobs` output is empty.
jobs > /scratch/jblist2
cat /scratch/jblist2 | wc -l > /scratch/jbcount2
assert jobs.empty 0 /scratch/jbcount2
