# Input redirection: `< file` points stdin at `file` for the first
# pipeline stage.
echo "line-one" > /scratch/in_src
echo "line-two" >> /scratch/in_src
echo "line-three" >> /scratch/in_src

# wc -l reading from the redirected stdin.
wc -l < /scratch/in_src > /scratch/in_wc
grep 3 /scratch/in_wc | wc -l > /scratch/in_wc_ok
assert redir.in.wc 1 /scratch/in_wc_ok

# Combined with a pipe: < feeds first stage, | chains the rest.
grep one < /scratch/in_src | wc -l > /scratch/in_pipe
grep 1 /scratch/in_pipe | wc -l > /scratch/in_pipe_ok
assert redir.in.pipe 1 /scratch/in_pipe_ok
