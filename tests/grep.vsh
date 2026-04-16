# grep with match, invert, count.
echo alpha > /scratch/g
echo beta >> /scratch/g
echo gamma >> /scratch/g

grep beta /scratch/g > /scratch/g1
assert grep.match beta /scratch/g1

grep -v beta /scratch/g | wc -l > /scratch/g2
assert grep.invert 2 /scratch/g2

grep -i ALPHA /scratch/g > /scratch/g3
assert grep.icase alpha /scratch/g3
