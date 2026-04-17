# test_fork — fork produces a distinct child pid; child exits 17;
# parent's waitpid returns that status.  The binary exits non-zero
# on any deviation, which fails run_script's any_fail gate.
test_fork
echo fork-ok > /scratch/tf.out
assert test_fork.ok fork-ok /scratch/tf.out
