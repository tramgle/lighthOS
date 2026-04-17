-- grep: match, invert, case-insensitive. Same coverage as grep.vsh
-- but expressed through testlib's t.eq / t.contains so failure
-- output carries the actual captured text instead of just an
-- expected/got mismatch against a scratch file.
local t = require('testlib')

t.sh('echo alpha > /scratch/g_lua')
t.sh('echo beta  >> /scratch/g_lua')
t.sh('echo gamma >> /scratch/g_lua')

t.eq      ('lua.grep.match',  'beta\n',  t.capture('grep beta /scratch/g_lua'))
t.contains('lua.grep.invert', t.capture('grep -v beta /scratch/g_lua'), 'alpha')
t.contains('lua.grep.invert.g', t.capture('grep -v beta /scratch/g_lua'), 'gamma')
t.eq      ('lua.grep.icase',  'alpha\n', t.capture('grep -i ALPHA /scratch/g_lua'))

t.done()
