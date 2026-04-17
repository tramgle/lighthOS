# /bin/time wraps a child, printing real/user/sys in 100 Hz ticks.
# Format from user/time.c: three lines "<label> N ticks".
time echo hello > /scratch/t.out
grep real /scratch/t.out | wc -l > /scratch/th_r
assert time.real-line 1 /scratch/th_r
grep user /scratch/t.out | wc -l > /scratch/th_u
assert time.user-line 1 /scratch/th_u
grep "sys " /scratch/t.out | wc -l > /scratch/th_s
assert time.sys-line 1 /scratch/th_s
