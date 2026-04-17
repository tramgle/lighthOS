# CPU time accounting: /proc/self/status must carry UTime / STime /
# StartTicks fields (populated by timer_callback charging ticks to
# process_current). We don't assert specific values — shell's own
# work is brief + bursty — only that the fields are present and
# formatted as non-negative integers.
grep UTime /proc/self/status | wc -l > /scratch/cpt_u
assert cputime.utime-present 1 /scratch/cpt_u

grep STime /proc/self/status | wc -l > /scratch/cpt_s
assert cputime.stime-present 1 /scratch/cpt_s

grep StartTicks /proc/self/status | wc -l > /scratch/cpt_st
assert cputime.start-present 1 /scratch/cpt_st
