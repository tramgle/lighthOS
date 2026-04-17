# Process group tests: setpgid / getpgid / kill-by-pgid.
/bin/test_pgroup > /scratch/pg_out
grep "pgroup ok" /scratch/pg_out | wc -l > /scratch/pg_ok
assert pgroup.ok 1 /scratch/pg_ok
