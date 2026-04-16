# M5 smoke test: run five converted dynamic binaries in sequence,
# confirm each produces its expected output. After M5, these all
# carry PT_INTERP and DT_NEEDED libulib.so.1.

echo bravo > /scratch/smoke_echo
assert smoke.echo bravo /scratch/smoke_echo

cat /scratch/smoke_echo > /scratch/smoke_cat
assert smoke.cat bravo /scratch/smoke_cat

echo -n hello | wc -c > /scratch/smoke_wc
assert smoke.wc 5 /scratch/smoke_wc

ls /bin | grep shell | wc -l > /scratch/smoke_ls
assert smoke.ls 1 /scratch/smoke_ls

sleep 0.05
echo done > /scratch/smoke_sleep
assert smoke.sleep done /scratch/smoke_sleep
