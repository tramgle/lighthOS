# XMM context-switch isolation: four concurrent tasks each load a
# unique XMM0 pattern and spin verifying it stays intact.
/bin/test_xmm > /scratch/xmm_out
grep "xmm ok" /scratch/xmm_out | wc -l > /scratch/xmm_ok
assert xmm.ok 1 /scratch/xmm_ok
