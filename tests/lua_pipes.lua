-- Pipe composition through libvibc's system() → /bin/shell -c. The
-- test mirrors pipes.vsh; value here is showing that shell pipes
-- still behave when the caller is a Lua host instead of /bin/shell.
local t = require('testlib')

t.eq      ('lua.pipes.wc',     '3\n',     t.capture('echo one two three | wc -w'))
t.eq      ('lua.pipes.triple', 'hello\n', t.capture('echo hello | cat | cat'))
t.contains('lua.pipes.hello',  t.capture('hello | wc -c'), '22')

t.done()
