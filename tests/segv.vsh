# Ring-3 NULL deref must terminate just the process, not panic the
# kernel. `||` swallows segv's non-zero exit so the script can carry
# on and assert on the pre-crash output captured through the pipe.
segv > /scratch/segv.out || echo caught > /scratch/segv.ok
assert segv.survived caught /scratch/segv.ok

grep 'about to' /scratch/segv.out | wc -l > /scratch/segv.pre
assert segv.preamble 1 /scratch/segv.pre
