# End-to-end dynamic-linking smoke test.
#
#   dynhello   — main exec, PT_INTERP=/lib/ld-lighthos.so.1, DT_NEEDED libulib.so.1
#   ld-lighthos.so.1 — ET_EXEC interpreter at 0x40000000 (statically links ulib)
#   libulib.so.1   — ET_DYN shared library, loaded into 0x30000000 region
#
# Success: dynhello prints "hello from dynamic". That single line proves:
#   - kernel loaded both main and interp
#   - SysV auxv passed correctly
#   - ld.so located and opened /lib/libulib.so.1
#   - DT_NEEDED + PT_LOAD + relocation dance all worked
#   - main's PLT slot for `puts` resolved to libulib.so.1's puts

/bin/dynhello > /scratch/dynout
grep "hello from dynamic" /scratch/dynout | wc -l > /scratch/dynok
assert dynhello.ran 1 /scratch/dynok
