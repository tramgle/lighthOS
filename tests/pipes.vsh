# Pipe assertions — verify multi-stage data flow and argv propagation.
echo one two three | wc -w > /scratch/pipes_wc
assert pipes.wc 3 /scratch/pipes_wc

echo hello | cat | cat > /scratch/pipes_triple
assert pipes.triple hello /scratch/pipes_triple

hello | wc -c > /scratch/pipes_hello_bytes
# "Hello from user space!" is 22 chars, no trailing newline
assert pipes.hello_bytes 22 /scratch/pipes_hello_bytes
