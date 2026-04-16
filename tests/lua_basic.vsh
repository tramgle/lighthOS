# Lua arithmetic + string.
lua -e "print(math.sqrt(16))" > /scratch/lua1
assert lua.sqrt 4.0 /scratch/lua1

lua -e "print(string.upper('ok'))" > /scratch/lua2
assert lua.upper OK /scratch/lua2

lua -e "local t = {1,2,3,4}; print(#t)" > /scratch/lua3
assert lua.tlen 4 /scratch/lua3
