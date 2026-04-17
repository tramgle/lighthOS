-- head / tail line bounds — same shape as head_tail.vsh but driven
-- by Lua so string comparisons happen inline instead of through
-- assert/grep/wc-l files.
local t = require('testlib')

-- Prep: four lines.
t.sh('echo one > /scratch/ht_lua')
t.sh('echo two >> /scratch/ht_lua')
t.sh('echo three >> /scratch/ht_lua')
t.sh('echo four >> /scratch/ht_lua')

-- head -n 1 → "one".
local first = t.capture('head -n 1 /scratch/ht_lua')
t.eq('lua.head.first', 'one\n', first)

-- tail -n 1 → "four".
local last = t.capture('tail -n 1 /scratch/ht_lua')
t.eq('lua.tail.last', 'four\n', last)

-- head -n 2 | wc -l → 2.
local n = t.capture('head -n 2 /scratch/ht_lua | wc -l')
t.contains('lua.head.count', n, '2')

t.done()
