# pkill smoke test. Backgrounded bombs tend to race with script mode
# (bombs are still alive when shell exits; the exit path's waitpid
# returns their signal-death exit code, which poisons run_script's
# any_fail latch). Instead: exercise pkill's argument parsing paths
# without actually spawning a background target. A separate manual
# run-bootdisk session covers the full kill-a-tree flow.

# -l lists signal names.
pkill -l > /scratch/pk_list
grep KILL /scratch/pk_list | wc -l > /scratch/pk_list_kill
assert pkill.list.kill 1 /scratch/pk_list_kill
grep TERM /scratch/pk_list | wc -l > /scratch/pk_list_term
assert pkill.list.term 1 /scratch/pk_list_term
grep INT /scratch/pk_list | wc -l > /scratch/pk_list_int
assert pkill.list.int 1 /scratch/pk_list_int
