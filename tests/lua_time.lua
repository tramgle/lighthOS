-- /bin/time output — three lines with fixed-form "<label> N ticks".
-- Pattern assertions beat the plain grep+wc-l shape in time.vsh
-- because a malformed tick field shows up in the FAIL detail.
local t = require('testlib')

local out = t.capture('time echo hello')
t.matches('lua.time.real', out, 'real%s+%d+%s+ticks')
t.matches('lua.time.user', out, 'user%s+%d+%s+ticks')
t.matches('lua.time.sys',  out, 'sys%s+%d+%s+ticks')

t.done()
