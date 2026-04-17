# Shell logical operators: ;, &&, ||, $?.
# Each segment runs through run_logops, gated by g_last_status.
# Any line that intentionally exits non-zero ends with `; true` so
# run_script's any_fail gate stays clean — we're exercising the
# operators, not propagating failures to the harness.

# `;` — both run, last one's status matters.
true; echo a > /scratch/lo_a
assert logops.semi a /scratch/lo_a

# `&&` — second only runs if first succeeded.
true && echo yes > /scratch/lo_and_ok
assert logops.and-pass yes /scratch/lo_and_ok
echo pre > /scratch/lo_and_skip
false && echo after > /scratch/lo_and_skip ; true
assert logops.and-skip pre /scratch/lo_and_skip

# `||` — second only runs if first failed.
echo pre > /scratch/lo_or_skip
true || echo after > /scratch/lo_or_skip
assert logops.or-skip pre /scratch/lo_or_skip
false || echo ran > /scratch/lo_or_run
assert logops.or-run ran /scratch/lo_or_run

# $? expansion after a success / failure. The trailing `; true`
# keeps any_fail clean while letting $? pick up the preceding
# command's status one line later.
true ; echo $? > /scratch/lo_dollar_0
assert logops.dollar-zero 0 /scratch/lo_dollar_0
false ; echo $? > /scratch/lo_dollar_1 ; true
assert logops.dollar-nonzero 1 /scratch/lo_dollar_1

# Chaining: && on success, || picks up a later failure.
true && echo first > /scratch/lo_chain ; false || echo second >> /scratch/lo_chain
grep first /scratch/lo_chain | wc -l > /scratch/lo_chain_f
assert logops.chain-first 1 /scratch/lo_chain_f
grep second /scratch/lo_chain | wc -l > /scratch/lo_chain_s
assert logops.chain-second 1 /scratch/lo_chain_s
