/* edges.rex: the conversion rules of ooRexx/Lua, edge cases (oorexx-bridges,
   topics/conversion-rules/lua.md). Run with lua.cls and librexxlua on
   REXX_PATH and the library path; see the header of lua.md. The master is
   oorexxlua's tests/probes (run by Probes.testGroup against edges.out); the
   copy in oorexx-bridges is a mirror. Addresses print as 0x... (noaddr, and
   describe), so that the output is the same on every run and platform. */
.lua~eval("function describe(x)" -
  "  local t = math.type(x) or type(x)" -
  "  if type(x) == 'string' then return t .. ':' .. string.format('%q', x) end" -
  "  if type(x) == 'table' then local p = {} for k, v in pairs(x) do p[#p + 1] = tostring(k) .. '=' .. describe(v) end" -
  "    table.sort(p) return t .. ':{' .. table.concat(p, ', ') .. '} #' .. #x end" -
  "  if type(x) == 'number' then return t .. ':' .. string.format(t == 'float' and '%.17g' or '%d', x) end" -
  "  return t .. ':' .. (tostring(x):gsub(': 0?x?%x+$', ': 0x...')) end")
say "--- Rexx -> Lua (arguments; results of Rexx callbacks follow the same rule)"
list = '0|-0|42|-7|007|+5| 5|5 |3.14|3.10|.5|5.|0.5|100.0|1E3|1e3|1E+3|1E-3|-1.5E2|1.0|9223372036854775807|9223372036854775808|12345678901234567890123456789|3.14159265358979323846264|0.333333333|1E999|1E-999|.|-|E3|1E|0x10|1_000|inf|NaN|abc||1 2'
do while list \= ""
  parse var list v "|" list
  say left("'"v"'", 34) .lua~eval("return describe(...)", v)
end
say left("12345678901234567890 * 10 (DIGITS 9)", 34) .lua~eval("return describe(...)", 12345678901234567890 * 10)
numeric digits 30
say left("2**100 under DIGITS 30", 34) .lua~eval("return describe(...)", 2**100)
numeric digits 9
say left("2**100 (DIGITS 9)", 34) .lua~eval("return describe(...)", 2**100)
say left(".true / .false", 34) .lua~eval("return describe(...)", .true) "/" .lua~eval("return describe(...)", .false)
say left(".false in a Lua condition", 34) .lua~eval("return (...) and 'truthy' or 'falsy'", .false)
say left(".lua~false in a Lua condition", 34) .lua~eval("return (...) and 'truthy' or 'falsy'", .lua~false)
say left(".Boolean~true / .Boolean~false", 34) .lua~eval("return describe(...)", .Boolean~true) "/" .lua~eval("return describe(...)", .Boolean~false)
say left(".Boolean~false in a Lua condition", 34) .lua~eval("return (...) and 'truthy' or 'falsy'", .Boolean~false)
say left(".lua~true == .Boolean~true", 34) (.lua~true == .Boolean~true) (.lua~true~class~id)
say left(".nil", 34) .lua~eval("return describe(...)", .nil)
a = .array~new(4); a[1] = 1; a[3] = "x"
say left("sparse Array [1,,x,] (size 4)", 34) .lua~eval("return describe(...)", a)
m = .array~new(2,2); m[1,1] = 1; m[2,2] = 4
say left("2-D Array 2x2", 34) .lua~eval("return describe(...)", m)
say left("nested ((1,2),(3,'a'))", 34) .lua~eval("return describe(...)", ((1,2),(3,"a")))
s.1 = "a"; s.0 = 1
say left("stem s.", 34) .lua~eval("return describe(...)", s.)
d = .directory~new; d~name = "x"
say left(".directory (NAME=x)", 34) .lua~eval("return describe(...)", d)
say left("s~table(.directory)", 34) .lua~eval("return describe(...)", .lua~table(d))
say left(".stringtable~new", 34) .lua~eval("return describe(...)", .stringtable~new)
say left(".list~of(1,2)", 34) .lua~eval("return describe(...)", .list~of(1,2))
say left(".mutablebuffer~new('12')", 34) .lua~eval("return describe(...)", .mutablebuffer~new("12"))
say left("'é' (UTF-8 bytes)", 34) .lua~eval("return describe(...)", "é")
say left("'ff'x", 34) .lua~eval("return describe(...)", "ff"x)
say left(".Routine", 34) .lua~eval("return describe(...)", .routine~new("r", "return 1"))
say left("box integer '007'", 34) .lua~eval("return describe(...)", .lua~box("integer", "007"))
say left("box float 3.10", 34) .lua~eval("return describe(...)", .lua~box("float", "3.10"))
say left("box string 42", 34) .lua~eval("return describe(...)", .lua~box("string", 42))
say left("box boolean 0", 34) .lua~eval("return describe(...)", .lua~box("boolean", 0))
arr = .array~of(1)
.lua~eval("local a = ... a[2] = 'added'", .lua~box("reference", arr))
say left("box reference: Lua a[2] = 'added'", 34) "Rexx sees items" arr~items "arr[2]=" || arr[2]
say "--- Lua -> Rexx"
e = "true|false|nil|0|-0|-0.0|math.maxinteger|math.mininteger|2^53|2^63|0.1+0.2|1e15|1e16|1e100|1e-7|1.0|100/3|1/0|-1/0|0/0|-(0/0)|'café'|''|'a\0b'|{1,2}|print|coroutine.create(print)|io.stdout"
do while e \= ""
  parse var e x "|" e
  r = .lua~eval("return" x)
  if r == .nil then say left(x, 34) ".nil"
  else if r~isA(.LuaObject) then say left(x, 34) "LuaObject:" noaddr(r~string)
  else say left(x, 34) "'"r~changestr("00"x, "\0")"'" "len" r~length "datatype" datatype(r)
end
say left("true, false: their class", 34) .lua~eval("return true")~class~id .lua~eval("return false")~class~id "(the instances:" (.lua~eval("return true") == .Boolean~true)")"
say left("float round trip: 0.1+0.2 back", 34) .lua~eval("return (...) == 0.1 + 0.2", .lua~eval("return 0.1 + 0.2"))
say left("1e16 + 1 in Rexx", 34) .lua~eval("return 1e16") + 1
say "--- Names, positions, identity"
t = .lua~eval("return {Name = 'Name', name = 'name', Other = 'Other', [1] = 'one'}")
say left("t~name (keys Name, name)", 34) t~name
say left("t~other (key Other)", 34) t~other
say left("t[1]", 34) t[1]
t2 = .lua~eval("local t = ... return t", t)
say left("table there and back: ==", 34) (t == t2)
o = .object~new
say left("Rexx object there and back: ==", 34) (.lua~eval("return ...", o) == o)
say left("t = t2 (Lua ==)", 34) (t = t2)
say "--- Errors"
signal on syntax name e1
.lua~eval("error('plain message')")
e1: c = condition("O")
say left("Lua error: code / rc", 34) c~code "/" c~rc
say left("  additional items", 34) c~additional~items
say left("  additional[1]", 34) c~additional[1]
say left("  additional[2] (the value)", 34) c~additional[2]
say left("  additional[3] (first line)", 34) c~additional[3]~makeArray("0a"x)[1]
say left("  message", 34) c~message
signal on syntax name e2
.lua~eval("error({code = 42})")
e2: c = condition("O")
say left("Lua error with a table", 34) noaddr(c~additional[1])
say left("  additional[2]~code", 34) c~additional[2]~code
r = .routine~new("r", "x = 'a' + 1")
say left("Rexx error 41.1 seen in Lua", 34) .lua~eval("local ok, e = pcall(...) return type(e) .. ' ' .. e.condition .. ' ' .. e.code .. ' rc ' .. e.rc .. ' line ' .. e.line .. ' | ' .. tostring(e)", r)
say left("  its traceback (first line)", 34) .lua~eval("local ok, e = pcall(...) return e.traceback", r)~makeArray("0a"x)[1]
say left("Rexx raise syntax 98.900 in Lua", 34) .lua~eval("local ok, e = pcall(...) return type(e) .. ' ' .. e.condition .. ' ' .. e.code .. ' rc ' .. e.rc .. ' line ' .. e.line .. ' | ' .. tostring(e)", .routine~new("q", "raise syntax 98.900 array('my message')"))
t3 = .lua~eval("return {}")
inner = .routine~new("i", "use arg f; return f~call")
say left("Lua table error through Rexx back", 34) .lua~eval("local f = function() error({code = 7}) end local ok, e = pcall(..., f) return describe(e)", inner)
signal on syntax name e3
.lua~eval("local r = ... r()", r)
e3: c = condition("O")
say left("Rexx error through Lua to Rexx", 34) c~code c~message
exit

-- s with the address after each "table: ", "function: ", "thread: ",
-- "userdata: " or "file (" as 0x... (Windows prints addresses without 0x)
::routine noaddr
  use strict arg s
  do prefix over "table: ", "function: ", "thread: ", "userdata: ", "file ("
    out = ""
    do forever
      p = s~pos(prefix)
      if p = 0 then leave
      q = p + prefix~length
      if s~substr(q, 2) == "0x" then q += 2
      r = q
      do while r <= s~length
        if verify(s~subchar(r), "0123456789abcdefABCDEF") \= 0 then leave
        r += 1
      end
      out ||= s~left(q - 1)
      if r > q & \datatype(s~subchar(r), "A") then do      -- (subchar past the end: "")
        out = out~left(out~length - (q - p - prefix~length)) || "0x..."
        s = s~substr(r)
      end
      else s = s~substr(q)
    end
    s = out || s
  end
  return s

::requires "lua.cls"
