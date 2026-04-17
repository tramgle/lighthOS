-- /proc reads — exercise the synthetic filesystem and the
-- /proc/self aliasing for the current pid. Pattern matching via
-- t.matches demonstrates testlib's Lua-pattern assertion.
local t = require('testlib')

local mi = t.capture('cat /proc/meminfo')
t.matches('lua.proc.memtotal', mi, 'MemTotal:%s+%d+')
t.matches('lua.proc.memfree',  mi, 'MemFree:%s+%d+')

local self_status = t.capture('cat /proc/self/status')
t.matches('lua.proc.self.pid',   self_status, 'Pid:%s+%d+')
t.matches('lua.proc.self.utime', self_status, 'UTime:%s+%d+')
t.matches('lua.proc.self.stime', self_status, 'STime:%s+%d+')

-- /proc root listing carries static entries.
local pl = t.capture('ls /proc')
t.contains('lua.proc.ls.meminfo', pl, 'meminfo')
t.contains('lua.proc.ls.mounts',  pl, 'mounts')

t.done()
