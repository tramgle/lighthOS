# Smoke test for SYS_MMAP_ANON + SYS_MPROTECT. /bin/mmaptest:
#   1. mmap 8KB RW at 0x30000000
#   2. confirm zero-initialized
#   3. write + read a pattern
#   4. assert a second overlap-mmap at the same addr fails
#   5. mprotect(PROT_READ) the first page; pattern still readable
#   6. mmap a second non-overlapping region
# On success it prints "OK <checksum>".

/bin/mmaptest > /scratch/mmapout
grep "OK " /scratch/mmapout | wc -l > /scratch/mmapok
assert mmap.ok 1 /scratch/mmapok
