# Dynamic linking through libvibc.so.1. dyn_echo uses snprintf from
# libvibc (which depends on libulib for strlen). This exercises:
#   - recursive DT_NEEDED: dyn_echo → libvibc → libulib
#   - symbol resolution across multiple loaded objects
#   - libvibc.so.1 being loaded via ld.so (not just libulib)

/bin/dyn_echo hello world > /scratch/dynecho
grep "hello world" /scratch/dynecho | wc -l > /scratch/dynecho_ok
assert dyn_echo.ran 1 /scratch/dynecho_ok
