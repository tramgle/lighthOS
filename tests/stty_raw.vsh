# stty raw / stty cooked wrap the new SYS_TTY_RAW ioctl. Just
# exercise the on/off path — the test ISO has no interactive stdin
# so we can't observe the echo suppression, only that the syscalls
# return 0 and don't destabilize the runtests driver.

stty raw
stty cooked
echo after-raw-cooked > /scratch/stty_r
grep after-raw-cooked /scratch/stty_r | wc -l > /scratch/stty_r_ok
assert stty.raw.cooked 1 /scratch/stty_r_ok
