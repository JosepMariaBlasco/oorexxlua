--[[ harness.lua: the tests written in Lua, of the Lua module (require
"oorexx"), use this.  tests/run.sh runs each tests/lua/*.test.lua with a
standalone lua, as ooRexxUnit's groups: it prints the same summary lines
(Tests ran, Assertions, Failures, Errors) and exits with a non-zero code if
anything failed.

  local T = dofile(...)              -- (run.sh passes this file's path in ORXLUA_HARNESS)
  T.test("name", function() T.eq(rexx.eval("6*7"), 42) end)
  T.done()

Licensed under the Apache License, Version 2.0. ]]

local T = {tests = 0, assertions = 0, failures = 0, errors = 0}

local Failure = {}                      -- the error value of a failed assertion

local function fail(msg, level)
  error(setmetatable({msg = msg}, Failure), level + 1)
end

local function show(v)
  if type(v) == "string" then return string.format("%q", v) end
  if math.type(v) then return math.type(v) .. " " .. tostring(v) end
  return tostring(v)
end

function T.eq(actual, expected, what)
  T.assertions = T.assertions + 1
  -- integers and floats differ here: 1 and 1.0 are not the same value for the bridge
  if actual ~= expected or math.type(actual) ~= math.type(expected) then
    fail((what and what .. ": " or "") .. "expected " .. show(expected) .. ", got " .. show(actual), 2)
  end
end

function T.ok(v, what)
  T.assertions = T.assertions + 1
  if not v then fail(what or "expected a true value", 2) end
end

-- f(...) must raise an error whose message contains `text` (plain)
function T.fails(text, f, ...)
  T.assertions = T.assertions + 1
  local ok, err = pcall(f, ...)
  if ok then fail("no error raised (expected " .. show(text) .. ")", 2) end
  err = tostring(err)
  if not err:find(text, 1, true) then fail("expected an error with " .. show(text) .. ", got " .. show(err), 2) end
end

function T.test(name, f)
  T.tests = T.tests + 1
  local ok, err = xpcall(f, function(e)
    if getmetatable(e) == Failure then return e end
    return debug.traceback(tostring(e), 2)
  end)
  if ok then return end
  if getmetatable(err) == Failure then
    T.failures = T.failures + 1
    print("[failure] " .. name .. ": " .. err.msg)
  else
    T.errors = T.errors + 1
    print("[error] " .. name .. ": " .. err)
  end
end

-- The resident memory of this process, in KB (0 if it cannot be measured):
-- /proc on Linux, ps on macOS, the working set on Windows (as LuaTestCase.cls)
function T.rss(rexx)
  local f = io.open("/proc/self/status")
  if f then
    local s = f:read("a"); f:close()
    return tonumber(s:match("VmRSS:%s+(%d+)")) or 0
  end
  local pid = rexx.eval("SysQueryProcess('PID')")
  local cmd = package.config:sub(1, 1) == "\\"
    and 'powershell -NoProfile -Command "[long]((Get-Process -Id ' .. pid .. ').WorkingSet64 / 1KB)"'
    or "ps -o rss= -p " .. pid
  local p = io.popen(cmd); local out = p:read("a"); p:close()
  return tonumber(out:match("%d+")) or 0
end

function T.done()
  print("Tests ran: " .. T.tests)
  print("Assertions: " .. T.assertions)
  print("Failures: " .. T.failures)
  print("Errors: " .. T.errors)
  os.exit(T.failures + T.errors == 0 and T.tests > 0, true)
end

return T
