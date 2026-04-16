# Environment-variables end-to-end test. envtest:
#   - setenv / getenv / unsetenv round-trip
#   - setenv + spawn /bin/env → child prints our marker
# The merged stdout contains both envtest's "OK" and /bin/env's
# dump of inherited vars.

/bin/envtest > /scratch/envout
grep "ENVTEST_MARKER=inherit" /scratch/envout | wc -l > /scratch/env_inherit
grep "OK" /scratch/envout | wc -l > /scratch/env_ok

assert env.inherit 1 /scratch/env_inherit
assert env.ok 1 /scratch/env_ok
