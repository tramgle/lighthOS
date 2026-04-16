# User-space SIGINT handler smoke test. /bin/sigtest installs a SIGINT
# handler, raises SIGINT against itself, then tests SIG_IGN and
# reinstall. On success it prints "CAUGHT 2" and exits 0.

/bin/sigtest > /scratch/sigout
assert signal.caught "CAUGHT 2" /scratch/sigout
