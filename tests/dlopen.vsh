# dlopen/dlsym smoke test. /bin/dlopentest opens /lib/libtestdl.so.1,
# looks up test_add + test_answer, calls them, verifies results.
# Exercises:
#   - ld.so's dlopen publishing the dl-ops table at 0x50000000
#   - libulib's dlopen/dlsym forwarding through that table
#   - runtime load of an ET_DYN with no DT_NEEDED of its own
#   - idempotent dlopen returning the same handle

/bin/dlopentest > /scratch/dlout
grep "OK" /scratch/dlout | wc -l > /scratch/dlok
assert dlopen.ok 1 /scratch/dlok
