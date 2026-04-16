# Smoke test the kernel syscall tracer. Run `echo hi` under strace,
# confirm at least one write() call was captured (echo's output) and
# the exit marker appears. Exact syscall counts vary as echo's
# startup path evolves, so don't assert on totals.

strace /bin/echo hi > /scratch/trace
grep "write(" /scratch/trace | wc -l > /scratch/trace_write
grep "exited" /scratch/trace | wc -l > /scratch/trace_exit

# At least one write call.
cat /scratch/trace_write > /scratch/trace_write_n
# Expect 1 or more; assert against the grep-then-wc pattern.
grep 0 /scratch/trace_write_n | wc -l > /scratch/trace_zero
assert strace.has_write 0 /scratch/trace_zero
assert strace.has_exit 1 /scratch/trace_exit
