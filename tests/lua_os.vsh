# Lua's os.execute + io.popen need libvibc's system() and popen() —
# both were stubs pre-batch-3. Verify end-to-end.

# os.execute: spawn /bin/echo through libc system(), check the exit
# status (0 on success).
lua -e "print(os.execute('echo hi'))" > /scratch/osx
grep true /scratch/osx | wc -l > /scratch/osx_ok
assert lua.os.execute 1 /scratch/osx_ok

# io.popen read: read the output of /bin/echo back into Lua and
# echo it. This exercises pipe + fork + execve + waitpid + FILE*.
lua -e "local p = io.popen('echo hello-popen'); print(p:read('*l')); p:close()" > /scratch/popen
grep hello-popen /scratch/popen | wc -l > /scratch/popen_ok
assert lua.io.popen 1 /scratch/popen_ok
