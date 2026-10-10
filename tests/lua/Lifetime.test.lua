--[[ Lifetime.test.lua: the Lua module's lifetime rules (docs/design.md,
*Lua → ooRexx* and *Lifetime*): references released both ways, memory flat
over many calls, and the host closing its state.
Run by tests/run.sh.  Licensed under the Apache License, Version 2.0. ]]

local T = dofile(os.getenv("ORXLUA_HARNESS"))
local rexx = require "oorexx"

-- "refs kept": the registry references Rexx's .LuaObjects hold, and the Rexx
-- objects Lua keeps alive
local function stats()
  local refs, kept = rexx.eval(".lua~'LUA.STATS'"):match("(%d+) (%d+)")
  return tonumber(refs), tonumber(kept)
end

-- Rexx garbage, so that ooRexx runs pending uninits; then a call drains them
local churn = rexx.routine("do i = 1 to 200000; a = .array~new(10); end")

T.test("Rexx objects that Lua drops are released", function()
  collectgarbage(); collectgarbage(); rexx.eval("1")
  local _, kept0 = stats()
  local keep = rexx.eval(".directory~new")
  local list = {}
  for i = 1, 1000 do list[i] = rexx.eval(".directory~new") end
  T.eq(select(2, stats()), kept0 + 1001)
  list = nil
  collectgarbage(); collectgarbage()
  rexx.eval("1")                                  -- finalized outside Rexx: let go at the next call
  T.eq(select(2, stats()), kept0 + 1)
  keep = nil
  collectgarbage(); collectgarbage(); rexx.eval("1")
  T.eq(select(2, stats()), kept0)
end)

T.test("Lua values that Rexx drops are released", function()
  churn(); churn()
  local refs0 = stats()
  local hold = rexx.routine("use arg t; .local~held = t")
  hold({})
  for i = 1, 1000 do rexx.eval("arg(1)~luaType", {i}) end
  churn(); churn()
  T.eq(stats(), refs0 + 1, "only .local~held's table")
  rexx.eval(".local~remove('HELD')")
  churn(); churn()
  T.eq(stats(), refs0)
end)

T.test("many calls leave the memory flat", function()
  local f = rexx.routine("return arg(1) + 1")
  for i = 1, 20000 do f(i) end                     -- warm up
  local before = T.rss(rexx)
  for i = 1, 1000000 do f(i) end
  for i = 1, 200000 do rexx.eval("arg(1)", i) end
  local d = rexx.eval(".directory~new")
  for i = 1, 200000 do d:put(i, "K") end
  local grown = (T.rss(rexx) - before) // 1024
  T.ok(grown < 100, "grew " .. grown .. " MB")
end)

-- A Lua that closes its state (lua.c does, at the end of a script):
-- the instances are terminated, so Rexx uninits run
T.test("the host closing its state terminates the instances", function()
  local lua = arg[-1]
  local script = T.tmpname("orxlua-close.lua")
  local f = io.open(script, "w")
  f:write([[
    local rexx = require "oorexx"
    local r = rexx.interpreter()
    rexx.routine("x = .Bye~new('default'); .local~x = x; ::class Bye; ::method init; expose n; use arg n;" ..
                 " ::method uninit; expose n; say 'bye' n")()
    t = {}
    rexx.routine("use arg t; .local~t = t")(t)
    print("end of script")
  ]])
  f:close()
  local p = io.popen(T.command('"' .. lua .. '" "' .. script .. '" 2>&1'))
  local out = p:read("a")
  local ok = p:close()
  os.remove(script)
  T.ok(ok, "exit status: " .. out)
  T.ok(out:find("end of script\nbye default", 1, true), out)
end)

T.done()
