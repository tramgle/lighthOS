# head / tail line bounds.
echo one > /scratch/ht
echo two >> /scratch/ht
echo three >> /scratch/ht
echo four >> /scratch/ht

head -n 1 /scratch/ht > /scratch/h1
assert head.first one /scratch/h1

tail -n 1 /scratch/ht > /scratch/t1
assert tail.last four /scratch/t1

head -n 2 /scratch/ht | wc -l > /scratch/h2
assert head.count 2 /scratch/h2
