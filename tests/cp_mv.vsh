# touch, cp, mv, rm on ramfs.
touch /scratch/a
echo seed > /scratch/a
cat /scratch/a > /scratch/a_out
assert cpmv.write seed /scratch/a_out

cp /scratch/a /scratch/b
cat /scratch/b > /scratch/b_out
assert cpmv.copied seed /scratch/b_out

mv /scratch/b /scratch/c
cat /scratch/c > /scratch/c_out
assert cpmv.moved seed /scratch/c_out

# Verify mv removed the source.
ls /scratch | grep "  b$" | wc -l > /scratch/b_gone
assert cpmv.b_gone 0 /scratch/b_gone
