--[[ 07-from-lua.lua -- the other direction: ooRexx inside Lua (require "oorexx").
   Its exact output is 07-from-lua.out.  Run: examples/run-one.sh 07-from-lua.lua
   (or any lua 5.3, 5.4 or 5.5 that finds oorexx.so, and lua.cls next to it) ]]

local rexx = require "oorexx"

-- 1. Rexx expressions, with arguments: arg(1), arg(2) ...
print(rexx.eval("6 * 7"), rexx.eval("arg(1) || '!'", "hello"))   -- 42 hello!
print(rexx.eval("reverse(arg(1))", "Lua"))                        -- auL

-- 2. Numbers arrive as numbers when nothing is lost; Rexx's formatting stays
print(math.type(rexx.eval("6 * 7")), rexx.eval("1/4"))           -- integer 0.25
print(rexx.eval("format(3.1, , 2)"), rexx.eval("2**100"))         -- 3.10 1.2676506e+30 (a float)
print(rexx.routine("numeric digits 40; return 2**100")())         -- 1267650600228229401496703205376

-- 3. Routines are Lua functions; Arrays come as tables
local times = rexx.routine("use arg a, b; return a * b")
print(times(6, 7))                                                 -- 42
local words = rexx.routine("return arg(1)~makeArray(' ')")("ooRexx in Lua")
print(#words, words[3])                                            -- 3 Lua

-- 4. Other Rexx objects stay Rexx objects: obj:message(args), obj.attr = v
local d = rexx.eval(".directory~new")
d:put("Rexx", "LANG")
d.name = "JM"                                                      -- d~name = "JM"
print(d:at("LANG"), d:name(), #d)                                  -- Rexx JM 2

-- 5. In Rexx, .lua is this Lua: its globals, its tables, its functions
greeting = "hello from Lua"
print(rexx.eval(".lua~global('greeting')~upper"))                 -- HELLO FROM LUA
local t = {3, 1, 2}
rexx.routine("use arg t; .lua~global('table')~sort(t)")(t)
print(table.concat(t, " "))                                        -- 1 2 3

-- 6. Rexx conditions are Lua errors: tables with the condition's fields,
--    whose tostring is the message
print(pcall(rexx.eval, "1 + 'a'"))
-- false Rexx error 41.1: Nonnumeric value ("a") used in arithmetic operation.
local ok, e = pcall(rexx.eval, "1 + 'a'")
print(e.condition, e.code, e.rc, e.errortext, e.line)            -- SYNTAX 41.1 41 Bad arithmetic conversion. 1
