-- testlib — assertion + shell helpers for LighthOS Lua tests.
--
-- Drop this on every test ISO at /lib/lua/testlib.lua; tests require
-- it with `local t = require('testlib')`. Each assertion prints a
-- PASS/FAIL line that runtests greps for:
--
--   t.eq(name, expected, actual)
--   t.contains(name, haystack, needle)
--   t.matches(name, haystack, lua_pattern)
--   t.truthy(name, value)
--
-- A script sets its exit code via t.done() at the end; any FAIL seen
-- flips the script's status to non-zero.
--
-- t.sh(cmd) runs `cmd` through libvibc's system() (fork + execve on
-- tokenized argv) and returns the exit status.
-- t.capture(cmd) runs `cmd` via io.popen and returns the full stdout.

local M = {}

local fail_count = 0

local function report(pass, name, detail)
    if pass then
        io.write('PASS ', name, '\n')
    else
        io.write('FAIL ', name)
        if detail then io.write(' (', detail, ')') end
        io.write('\n')
        fail_count = fail_count + 1
    end
    io.flush()
end

function M.eq(name, expected, actual)
    local ok = expected == actual
    local detail = nil
    if not ok then
        detail = string.format('expected=%s got=%s',
            tostring(expected), tostring(actual))
    end
    report(ok, name, detail)
end

function M.truthy(name, v)
    report(v and true or false, name,
        v == nil and 'nil' or tostring(v))
end

function M.contains(name, hay, needle)
    local ok = hay and needle and hay:find(needle, 1, true) ~= nil
    report(ok, name,
        not ok and string.format("needle=%q not in %q",
            tostring(needle), tostring(hay)) or nil)
end

function M.matches(name, hay, pat)
    local ok = hay and pat and hay:find(pat) ~= nil
    report(ok, name,
        not ok and string.format("pattern=%q no match in %q",
            tostring(pat), tostring(hay)) or nil)
end

function M.sh(cmd) return os.execute(cmd) end

function M.capture(cmd)
    local p = io.popen(cmd, 'r')
    if not p then return nil end
    local s = p:read('*a') or ''
    p:close()
    return s
end

-- Call at end of every test file. Exits non-zero if any FAIL landed.
function M.done()
    if fail_count > 0 then os.exit(1) end
    os.exit(0)
end

return M
