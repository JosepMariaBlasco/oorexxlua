-- edges-module.lua: the same rule seen from Lua, through require "oorexx"
-- (oorexx-bridges, topics/conversion-rules/lua.md; the master is oorexxlua's
-- tests/probes, run by Probes.testGroup against edges-module.out)
local rexx = require "oorexx"
for _, x in ipairs{"'3.10'", "3.10", "'007'", "42", "1.5", ".true", ".Boolean~false", ".nil", "1E3", "2**100"} do
  local v = rexx.eval(x)
  print(x, math.type(v) or type(v), v)
end
print("true through Rexx:", type(rexx.eval("arg(1)", true)), rexx.eval("arg(1)~class~id", false))
local ok, e = pcall(rexx.eval, "'a' + 1")
print("error:", type(e), e)
local f = rexx.routine("use arg x; return x")
print("routine(3.10):", math.type(f("3.10")) or type(f("3.10")), f("3.10"))
