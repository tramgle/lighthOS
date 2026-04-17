# test_stream — sys_write accepts a full 256-byte buffer in one call;
# sys_getpid / sys_time handle 32 back-to-back invocations.
test_stream > /scratch/ts.out
echo stream-ok > /scratch/ts2.out
assert test_stream.ok stream-ok /scratch/ts2.out
