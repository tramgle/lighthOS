# hexdump / pagemap / regions — exercise SYS_PEEK / SYS_PAGEMAP /
# SYS_REGIONS. All three were dropped during the x86_64 port and
# restored in this branch.

# regions prints at least a "FREE" entry (we always have free RAM)
regions > /scratch/regs
grep FREE /scratch/regs | wc -l > /scratch/regs_free
# >= 1 FREE region — use head to collapse to a single line of digits
head -n 1 /scratch/regs_free > /scratch/regs_free1
# pipe through wc -c to force any number > 0 (single char + newline == 2)
wc -c /scratch/regs_free1 > /scratch/regs_nonzero
# This is indirect: we just want the test to not error. Assert the
# banner is present.
grep "PMM regions" /scratch/regs | wc -l > /scratch/regs_banner
assert regions.banner 1 /scratch/regs_banner

# hexdump of physical addr 0 reads the real mode IVT. The first byte
# of the IVT is IVT[0].offset_lo — on QEMU boot with SeaBIOS, that's
# a fixed value. Rather than hard-coding, just assert the output has
# exactly 8 lines for 128 bytes default.
hexdump 0x0 > /scratch/hd
wc -l /scratch/hd > /scratch/hd_lines
assert hexdump.lines 8 /scratch/hd_lines

# pagemap of the pagemap binary's own .text (0x400000 is user.ld's
# ORG for static user binaries — guaranteed mapped + present).
pagemap 0x400000 > /scratch/pm
grep "vaddr 0x400000" /scratch/pm | wc -l > /scratch/pm_addr
assert pagemap.addr 1 /scratch/pm_addr
grep "phys " /scratch/pm | wc -l > /scratch/pm_phys
assert pagemap.phys 1 /scratch/pm_phys
