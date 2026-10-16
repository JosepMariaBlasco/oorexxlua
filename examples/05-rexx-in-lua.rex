/* 05-rexx-in-lua.rex -- Rexx objects inside Lua: messages, [], #, pairs.
   Its exact output is 05-rexx-in-lua.out.  Run: examples/run-one.sh 05-rexx-in-lua.rex */

-- 1. A string key is a message: obj:name(...) or obj.name(...) sends it,
--    obj.name = v sends NAME=
p = .Point~new(3, 4)
say .lua~eval("local p = ...; return p:x() .. ',' .. p.y() .. ' ' .. p:norm2()", p)   -- 3,4 25
say .lua~eval("local p = ...; p.x = 6; p.y = 8; return p:norm2()", p)   -- 100
say p~x p~y                                        -- 6 8: the Rexx object itself changed

-- 2. Any other key goes to Rexx's [] and []=, and # is ~items: Rexx
--    collections work with Lua's own idioms
a = .array~of("one", "two", "three")
show = .lua~eval("function(a) local r = {} for i, v in ipairs(a) do" -
                 " r[#r + 1] = i .. '=' .. v end return table.concat(r, ' ') end")
say show~call(.lua~box("reference", a))            -- 1=one 2=two 3=three
.lua~eval("local a = ...; a[4] = 'four'; return #a", .lua~box("reference", a))
say a~items a[4]                                   -- 4 four

-- 3. pairs goes through the object's supplier
d = .directory~new; d~name = "JM"; d~lang = "Rexx"
keys = .lua~eval("function(d) local r = {} for k, v in pairs(d) do" -
                 " r[#r + 1] = k .. ':' .. v end table.sort(r) return table.concat(r, ' ') end")
say keys~call(d)                                   -- LANG:Rexx NAME:JM
say .lua~eval("local d = ...; return d:at('NAME') .. ' ' .. #d", d)   -- JM 2: string keys go by message

-- 4. A Rexx error inside Lua is a Lua error (a table with its fields), and
--    back in Rexx it is that Rexx error again
say .lua~eval("local p = ...; local ok, e = pcall(p.nonsense, p); return e.condition .. ' ' .. e.code", p)   -- SYNTAX 97.1
signal on syntax name e1
say .lua~eval("local a = ...; return a[0]", .lua~box("reference", a))
e1: say "Rexx catches:" condition("O")~code condition("O")~message
signal on syntax name e2
say .lua~eval("local p = ...; return p:nonsense()", p)
e2: say "Rexx catches:" condition("O")~code condition("O")~message
exit

::requires "lua.cls"

::class Point
::attribute x
::attribute y
::method init
  expose x y
  use arg x, y
::method norm2
  expose x y
  return x * x + y * y
