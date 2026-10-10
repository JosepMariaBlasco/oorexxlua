--[[ check.lua: does the Lua module of ooRexx/Lua (require "oorexx") work?
  lua check.lua             (a Lua of this package's version: lua5.4 for a
                             lua5.4 package; texlua for a lua5.3 one)
Needs this folder in LUA_CPATH (oorexx.so, oorexx.dll; lua.cls is next to
it), and ooRexx's libraries where the system finds them (README.md). ]]

local ok, rexx = pcall(require, "oorexx")
if not ok then
  print("ooRexx/Lua module: cannot load it: " .. tostring(rexx))
  os.exit(1)
end
print("ooRexx/Lua module: " .. _VERSION .. ", ooRexx " .. rexx.eval(".rexxInfo~version"))
local good = rexx.eval("6 * 7") == 42                                -- a number
  and rexx.eval("format(3.1, , 2)") == "3.10"                       -- Rexx's formatting
  and rexx.routine("use arg t; return .lua~global('table')~concat(t, ',')")({1, 2}) == "1,2"
print(good and "ooRexx/Lua module: OK" or "ooRexx/Lua module: FAILED")
os.exit(good and 0 or 1, true)
