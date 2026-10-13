--[[ Module.test.lua: the Lua module, require "oorexx" (docs/design.md,
*Lua → ooRexx*): values both ways, eval, run, routine, errors, the host's
state as .lua, coroutines, nesting, interpreter instances, threads.
Run by tests/run.sh.  Licensed under the Apache License, Version 2.0. ]]

local T = dofile(os.getenv("ORXLUA_HARNESS"))
local HERE = os.getenv("ORXLUA_TESTS")             -- tests/lua
local rexx = require "oorexx"

T.test("eval: Rexx values in Lua, by the rule that loses nothing", function()
  T.eq(rexx.eval("6 * 7"), 42)
  T.eq(rexx.eval("-7"), -7)
  T.eq(rexx.eval("1/4"), 0.25)
  T.eq(rexx.eval("1/3"), 0.333333333)
  T.eq(rexx.eval("format(3.1, , 2)"), "3.10")       -- a float would lose the 0
  T.eq(rexx.eval("1E3"), "1E3")
  T.eq(rexx.eval("2**100"), "1.2676506E+30")
  T.eq(rexx.eval("'007'"), "007")
  T.eq(rexx.eval("12345678901234567890"), "12345678901234567890")   -- not in 64 bits
  T.eq(rexx.eval(".nil"), nil)
  T.eq(rexx.eval("'abc'~reverse"), "cba")
end)

T.test("eval: arguments are arg(1), arg(2) ...", function()
  T.eq(rexx.eval("arg(1) + arg(2)", 40, 2), 42)
  T.eq(rexx.eval("arg()", 1, nil, 3), 3)
  T.eq(rexx.eval("arg(2, 'O')", 1, nil, 3), 1)       -- nil: an omitted argument
  T.eq(rexx.eval("arg(1) arg(2)", true, false), "1 0")
  T.eq(rexx.eval("arg(1)", 2^10), 1024.0)            -- a float makes the round trip
  T.eq(rexx.eval("arg(1)", 0.1 + 0.2), 0.1 + 0.2)
end)

T.test("Arrays come as tables; other objects by reference", function()
  local a = rexx.eval(".array~of(1, 'b', 2.5)")
  T.eq(type(a), "table"); T.eq(#a, 3); T.eq(a[1], 1); T.eq(a[2], "b"); T.eq(a[3], 2.5)
  local d = rexx.eval(".directory~new")
  T.eq(type(d), "userdata")
  d.name = "Ann"                                     -- NAME=
  T.eq(d:name(), "Ann")
  T.eq(d:items(), 1); T.eq(#d, 1)
  T.eq(rexx.eval("arg(1)~name", d), "Ann")
  T.eq(rexx.eval("arg(1)", d), d)                    -- the same object, the same userdata
  T.eq(tostring(d), "a Directory")
end)

T.test("Lua values in Rexx are .LuaObjects of this Lua", function()
  local t = {10, 20, 30, name = "t"}
  T.eq(rexx.eval("arg(1)~luaType", t), "table")
  T.eq(rexx.eval("arg(1)[2]", t), 20)
  T.eq(rexx.eval("arg(1)~name", t), "t")
  T.eq(rexx.eval("arg(1)", t), t)                    -- back as the same table
  rexx.routine("use arg t; t[4] = 40")(t)
  T.eq(t[4], 40)
  T.eq(rexx.eval("arg(1)~concat(arg(2), '-')", table, t), "10-20-30-40")
  T.eq(rexx.eval("arg(1)~call(6)", function(x) return x * 7 end), 42)
end)

T.test(".lua is the state of the Lua that runs ooRexx: its globals", function()
  answer = 42
  T.eq(rexx.eval(".lua~global('answer')"), 42)
end)

T.test(".lua is the state of the Lua that runs ooRexx: eval", function()
  rexx.eval(".lua~eval('fromRexx = ...', 'hi')")
  T.eq(fromRexx, "hi")
end)

T.test(".lua cannot be closed from Rexx", function()
  T.fails("closed by that Lua, not by Rexx", rexx.eval, ".lua~close")
  T.eq(rexx.eval(".lua~isClosed"), 0)
end)

T.test("routine: a Rexx routine as a Lua function", function()
  local times = rexx.routine("use arg a, b; return a * b")
  T.eq(type(times), "function")
  T.eq(times(6, 7), 42)
  T.eq(table.concat({3, 1, 2}, ","), "3,1,2")
  local t = {3, 1, 2}
  table.sort(t, rexx.routine("return arg(1) < arg(2)") and function(a, b)
    return rexx.routine("return arg(1) < arg(2)")(a, b) == 1 end)
  T.eq(table.concat(t, ","), "1,2,3")
  T.fails("Rexx error 40.", rexx.routine("return abs()", "MINE"))
  local r = rexx.routine("return .LuaState~new~eval('return 6 * 7')")   -- lua.cls's classes are there
  T.eq(r(), 42)
end)

T.test("run: CALL a program", function()
  local r = rexx.interpreter{rexx_path = HERE}
  T.eq(r.run("hello.rex", "world"), "hello world")
  T.eq(r.run(HERE .. "/hello.rex", "you"), "hello you")
  r.close()
  T.fails("Rexx error 3.901", rexx.run, "no-such-program.rex")
end)

T.test("Rexx conditions are Lua errors", function()
  T.fails("Rexx error 41.1", rexx.eval, "1 + 'a'")
  T.fails("Rexx error 35.1", rexx.eval, "1 +")
  T.fails("Rexx error 43.1", rexx.eval, "nosuchfunction(1)")
  local ok, err = pcall(rexx.routine("raise syntax 98.900 array ('mine')"))
  T.ok(not ok); T.ok(tostring(err):find("mine", 1, true), err)
  ok, err = pcall(rexx.routine("raise syntax 98.900 array ('a' || '00'x || 'b')"))
  T.ok(not ok); T.ok(tostring(err):find("a\0b", 1, true), "a NUL in the message stays")
  -- and Lua errors inside Rexx come back as Lua errors
  T.fails("boom", rexx.eval, ".lua~eval(\"error('boom')\")")
end)

T.test("callbacks both ways, nested", function()
  function twice(f, v) return f(f(v)) end
  T.eq(rexx.eval(".lua~global('twice')~call(arg(1), 3)", rexx.routine("return arg(1) * 10")), 300)
  local depth = rexx.routine([[
    use arg n, again
    if n = 0 then return 0
    return 1 + again~call(n - 1, again)
  ]])
  T.eq(depth(20, depth), 20)
end)

T.test("coroutines", function()
  local co = coroutine.wrap(function(x)
    local y = coroutine.yield(rexx.eval("arg(1) * 2", x))
    return rexx.eval("arg(1) + 1", y)
  end)
  T.eq(co(5), 10)
  T.eq(co(41), 42)
  -- a Rexx object called from a coroutine
  local d = rexx.eval(".directory~new")
  local co2 = coroutine.create(function() d:put("v", "K"); coroutine.yield(); return d:at("K") end)
  coroutine.resume(co2)
  local _, v = coroutine.resume(co2)
  T.eq(v, "v")
end)

T.test("require 'oorexx' in Lua that Rexx runs: the running instance", function()
  T.eq(rexx.eval(".lua~eval(\"return require('oorexx') == ...\", arg(1))", rexx), 1)
  T.eq(rexx.eval([[.LuaState~new~eval("return require('oorexx').eval('2 + 2')")]]), 4)
  T.eq(rexx.eval([[.LuaState~new~eval("return require('oorexx').interpreter")]]), nil)
end)

T.test("interpreter: other instances, with options, and close", function()
  local r = rexx.interpreter{rexx_path = {HERE, "/nonexistent"}, extensions = ".rex",
                             initial_address = "MINE"}
  T.eq(r.eval("address()"), "MINE")
  T.eq(r.eval("6 * 7"), 42)
  T.eq(r.eval(".lua~global('answer')"), 42)          -- the same Lua
  T.eq(r.run("hello", "there"), "hello there")       -- the extension
  local f = r.routine("return 'from r'")
  r.close()
  r.close()                                           -- harmless
  T.fails("closed", r.eval, "1")
  T.eq(f(), "from r")                                -- its objects live on (another instance runs them)
  T.fails("unknown option", rexx.interpreter, {rexxpath = "x"})
  T.fails("must be a string", rexx.interpreter, {rexx_path = {1}})
  local r2 = rexx.interpreter()
  T.fails("while its Rexx code is running", r2.eval, ".lua~eval('return (...)()', arg(1))", r2.close)
  r2.close()
end)

T.test("calls from Lua run in the instance that made the routine or object", function()
  local other = rexx.interpreter{initial_address = "OTHER"}
  local where = rexx.routine("return address()")
  local mine = rexx.routine(".local~mark = 'default'")
  mine()
  T.ok(rexx.eval("address()") ~= "OTHER")
  T.eq(where(), rexx.eval("address()"))
  T.eq(rexx.eval(".local~mark"), "default")
  T.eq(other.routine("return address()")(), "OTHER")
  T.eq(rexx.eval("arg(1)~'LUA.STATE' == .lua", {}), 1)
  other.close()
  T.eq(where(), rexx.eval("address()"))                 -- its instance is alive
end)

T.test("after a Lua error inside a call, the instance is not left busy", function()
  local r = rexx.interpreter()
  T.ok(not pcall(r.routine("x = 1 + 'a'")))
  local o = r.eval(".object~new")
  T.ok(not pcall(function() o:nosuchmessage() end))
  if _VERSION == "Lua 5.3" then                          -- 5.3 raises a finalizer's error in the allocation
    local f = r.routine("return .array~new(100000)~fill(1)")
    setmetatable({}, {__gc = function() error("finalizer failed") end})
    for _ = 1, 3 do pcall(f) end
  end
  r.close()                                              -- not "while its Rexx code is running"
  T.ok(true)
end)

T.test(".LuaState~new ignores its arguments", function()
  T.eq(rexx.eval(".LuaState~new(42)~eval('1 + 1')"), 2)
  T.eq(rexx.eval(".LuaState~new(.nil)~eval('1 + 1')"), 2)
end)

T.test("interpreter options are read raw (an __index never runs)", function()
  local opts = setmetatable({}, {__index = function() error("__index ran") end})
  local r = rexx.interpreter(opts)
  T.eq(r.eval("6 * 7"), 42)
  r.close()
end)

T.test("another Rexx thread cannot use the host's state; a state of its own, yes", function()
  T.eq(rexx.routine([[
    m = .Message~new(.lua, "EVAL", "I", "return 1")
    m~start
    m~wait
    if m~hasError then return m~errorCondition~message
    return "no error"
  ]])(), "Lua error: the LuaState of the Lua that runs ooRexx can only be used from that Lua's thread.")
  T.eq(rexx.routine([[
    m = .Message~new(.LuaState~new, "EVAL", "I", "return 6 * 7")
    m~start
    return m~result
  ]])(), 42)
end)

T.test("ADDRESS LUA runs in the host's state, in every instance", function()
  addressCount = 0
  T.eq(rexx.routine([[
    address lua 'addressCount = addressCount + 1'
    address lua 'return addressCount * 10'; r = result
    return rc r
  ]])(), "0 10")
  T.eq(addressCount, 1)
  local other = rexx.interpreter{initial_address = "LUA"}
  T.eq(other.routine("'addressCount = 100'; return rc")(), 0)
  T.eq(addressCount, 100)
  T.eq(other.routine([[
    signal on error name e
    'error("boom")'
    return "no ERROR"
    e: return rc condition('A')~makeArray('0a'x)[1]
  ]])(), "1 ADDRESS LUA:1: boom")
  other.close()
  T.eq(rexx.routine([[
    m = .Message~new(.Address, "RUN")
    m~start
    m~wait
    return m~result
    ::class Address
    ::method run class
      signal on failure name f
      address lua '1'
      return "no FAILURE"
      f: return rc condition('A')
  ]])(), "-2 the LuaState of the Lua that runs ooRexx can only be used from that Lua's thread")
  addressCount = nil
end)

-- needs an ooRexx with the fixes in patches/oorexx/ (tests/run.sh)
if os.getenv("ORXLUA_OOREXX_FIXES") == "1" then
  T.test("ADDRESS LUA ... WITH, in the host's state and in another instance", function()
    local before = print
    T.eq(rexx.routine([[
      in.0 = 2; in.1 = "3"; in.2 = "4"
      address lua 'local s = 0 for l in io.lines() do s = s + tonumber(l) end print("sum", s)' -
        with input stem in. output stem out.
      return rc out.0 out.1
    ]])(), "0 1 sum\t7")
    T.eq(print, before, "print is Lua's again")
    local other = rexx.interpreter{initial_address = "LUA"}
    T.eq(other.routine([[
      address lua 'io.write("a"); io.stderr:write("e\n"); io.write("b\n")' with output stem o. error stem e.
      return o.1 e.1
    ]])(), "ab e")
    other.close()
    T.eq(print, before)
  end)
end

T.done()
