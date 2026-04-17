# test_pid — verify sys_getpid returns a positive pid and
# sys_write round-trips the "pid-ok\n" greeting back to stdout.
test_pid > /scratch/test_pid.out
assert test_pid.ok pid-ok /scratch/test_pid.out
