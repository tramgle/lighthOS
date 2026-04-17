# ls sorts entries alphabetically regardless of readdir order.
# Piped stdout -> one-per-line layout, so `head -1` / `tail -1`
# give the first / last sorted names.
mkdir /scratch/ls_sort
touch /scratch/ls_sort/charlie
touch /scratch/ls_sort/alpha
touch /scratch/ls_sort/bravo
touch /scratch/ls_sort/delta

ls /scratch/ls_sort | head -n 1 > /scratch/ls_first
assert ls.sort.first alpha /scratch/ls_first

ls /scratch/ls_sort | tail -n 1 > /scratch/ls_last
assert ls.sort.last delta /scratch/ls_last

ls /scratch/ls_sort | wc -l > /scratch/ls_count
assert ls.sort.count 4 /scratch/ls_count

# -1 flag forces one-per-line even in an interactive ls; here it's
# indistinguishable from the default since we're piping, but the
# flag must at least be accepted.
ls -1 /scratch/ls_sort | wc -l > /scratch/ls_one
assert ls.one.count 4 /scratch/ls_one

# -C forces columns even when not a tty: four short names fit one
# row of four columns → a single output line.
ls -C /scratch/ls_sort | wc -l > /scratch/ls_cols
assert ls.cols.rows 1 /scratch/ls_cols
