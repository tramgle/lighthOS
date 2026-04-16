# SIGALRM + alarm() end-to-end test. /bin/alarmtest:
#   1. registers a SIGALRM handler,
#   2. sets alarm(1) then alarm(0) — handler must NOT fire,
#   3. sets alarm(1) and busy-yields; handler must fire with
#      elapsed ticks roughly 100 (range [80, 150]).
# On success it prints "OK <ticks>". We grep for the "OK" prefix so
# minor jitter in the tick count doesn't fail the test.

/bin/alarmtest > /scratch/alrmout
# Our grep is literal substring, no regex anchors. "OK " is distinct
# enough; other output lines are "A1"/"A2"/"A3 prev=…" or "FAIL …".
grep "OK " /scratch/alrmout | wc -l > /scratch/alrmok
assert alarm.fired 1 /scratch/alrmok
