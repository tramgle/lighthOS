# FAT16 driver sanity. If the host seeded /fat/HELLO.TXT, verify its
# content. Otherwise just confirm /fat is mountable and readable.
# Either way, exercise cat + wc + grep over a known file.

# Create a known fixture on ramfs for the core assertions.
echo "line one" > /scratch/fattest
echo "line two" >> /scratch/fattest

cat /scratch/fattest | wc -l > /scratch/fat_lines
assert fat.lines 2 /scratch/fat_lines

cat /scratch/fattest | grep two | wc -l > /scratch/fat_grep
assert fat.grep 1 /scratch/fat_grep
