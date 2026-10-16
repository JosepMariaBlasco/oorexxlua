/* 01-tour.rex -- Lua embedded in ooRexx, one feature per section.
   Its exact output is 01-tour.out.  Run: examples/run-one.sh 01-tour.rex  */

-- 1. Evaluating, and passing arguments
say .lua~eval("2^10")                              -- 1024.0 (a float in 5.4)
say .lua~eval("1 << 10")                           -- 1024
say .lua~eval("... .. '!'", "hello")               -- hello!
say .lua~eval("select('#', ...)", 1, 2, 3)         -- 3

-- 2. Libraries: string.rep is a module function (no self)
string = .lua~global("string")
say string~rep("ab", 3, "-")                       -- ab-ab-ab
say string~upper("lua")                            -- LUA
math = .lua~global("math")
say math~pi math~maxinteger                        -- 3.141592653589793 9223372036854775807

-- 3. Several results
find = string~func("find", "hello world", "o w")   -- the first one only
say find
say string~get("find")~callAll("hello world", "o w")~toString(, ",")   -- 5,7

-- 4. Tables: 1-based, as in Rexx
t = .lua~table(.array~of("one", "two", "three"))
say t[2] t~luaType                                 -- two table
t[4] = "four"
do x over t; say ">" x; end
say .lua~global("table")~concat(t, "+")            -- one+two+three+four

-- 5. Objects: o~name is o:name(...) when the function has a self parameter
.lua~eval("Counter = {}; Counter.__index = Counter;" -
  "function Counter.new(n) return setmetatable({n = n or 0}, Counter) end;" -
  "function Counter:inc(k) self.n = self.n + (k or 1); return self.n end")
c = .lua~global("Counter")~new(10)                 -- Counter.new(n): no self, '.'
say c~inc c~inc(5) c~n                             -- 11 16 16 (Counter:inc: ':')
s = .lua~eval("'abc'")                             -- a Lua string comes back as a Rexx string

-- 6. Booleans: in Lua, 0 is true
say .lua~eval("(...) and 'true' or 'false'", 0)            -- true!
say .lua~eval("(...) and 'true' or 'false'", .lua~false)   -- false

-- 7. Callbacks: Lua calling Rexx
byLength = .routine~new("cmp", "return length(arg(1)) < length(arg(2))")
words = .lua~table(.array~of("ccc", "a", "bb", "dddd"))
signal on syntax name e0
.lua~global("table")~sort(words, byLength)         -- "0" is true in Lua...
e0: say "Without predicate:" condition("O")~message
signal off syntax
.lua~global("table")~sort(words, .lua~predicate(byLength))
say .lua~global("table")~concat(words, " ")        -- a bb ccc dddd
-- a Rexx object inside Lua, with methods
d = .directory~new; d~name = "JM"
say .lua~eval("local d = ...; return d:at('NAME') .. ' / ' .. tostring(d)", d)

-- 8. Errors, both ways
signal on syntax name e1
say .lua~eval("error('boom')")                     -- a Lua error: SYNTAX 98.900
e1: say "Rexx catches:" condition("O")~code condition("O")~additional[2]
signal on syntax name e2                           -- a Rexx error through Lua: itself
say .lua~eval("local f = ...; return f()", .routine~new("bad", "return 1 + 'a'"))
e2: say "Rexx catches:" condition("O")~code condition("O")~message
exit

::requires "lua.cls"
