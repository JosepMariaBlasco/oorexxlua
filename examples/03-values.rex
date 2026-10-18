/* 03-values.rex -- how values cross: numbers that lose nothing, exact floats, box.
   Its exact output is 03-values.out.  Run: examples/run-one.sh 03-values.rex  */

-- 1. Rexx -> Lua: canonical integers, and Rexx numbers with a point that a
--    Lua float writes back the same, are Lua numbers; anything else is a
--    string, so that nothing is lost
types = .lua~eval("function(...) local r = {}; for i = 1, select('#', ...) do" -
                  " local v = select(i, ...); r[i] = math.type(v) or type(v) end;" -
                  " return table.concat(r, ' ') end")
say types~call(42, -7, 0, 3.14, 0.5, "100.0")      -- integer integer integer float float float
say types~call("3.10", "1E3", ".5", 2**100)        -- string string string float
                                                   -- (2**100 is 1.2676506E+30, as a float writes it)
say types~call("007", " 5", "+5", "abc", "1.2.3")  -- string string string string string
say types~call(99999999999999999999)               -- string: does not fit in 64 bits
say .lua~eval("... .. ' EUR'", format(3.1, , 2))   -- 3.10 EUR: Rexx's formatting arrives

-- 2. So Lua can compare them, not only add them
positive = .lua~eval("function(n) return n > 0 end")
say positive~call(5) positive~call(-5)             -- 1 0
say .lua~global("math")~max(3, 7, 5)               -- 7
t = .lua~table
t[1] = "one"                                       -- the key 1 is the integer 1
say .lua~eval("return (...)[1]", t)                -- one

-- 3. Lua -> Rexx: integers as they are, floats with every digit they need
say .lua~eval("1 + 2") .lua~eval("1.0 + 2") .lua~eval("2^10")   -- 3 3.0 1024.0
say .lua~eval("0.1 + 0.2")                         -- 0.30000000000000004
say .lua~eval("1/3")                               -- 0.3333333333333333
say .lua~eval("1e100") .lua~eval("1e-7")           -- 1E+100 0.0000001: as Rexx writes them
say .lua~eval("-0.0") .lua~eval("1/0") .lua~eval("-1/0") .lua~eval("0/0")   -- -0.0 Infinity -Infinity NaN
x = .lua~eval("1/3")
say .lua~eval("return ... == 1/3", x)              -- 1: a float comes back exact

-- 4. box: when the rule does not give the type you need
say types~call(.lua~box("string", 42), .lua~box("float", 2), .lua~box("integer", "007"))
                                                   -- string float integer
say .lua~eval("(...) and 'true' or 'false'", .lua~box("boolean", 0))   -- false
a = .array~of("x")
.lua~eval("local a = ...; a:append('y')", .lua~box("reference", a))
say a~toString(, ",")                              -- x,y: the Rexx Array itself
.lua~eval("local a = ...; a[2] = 'z'", a)          -- an Array alone goes as a copy
say a~toString(, ",")                              -- x,y
signal on syntax name e1
say .lua~box("integer", 1.5)
e1: say "Error:" condition("O")~message
signal on syntax name e2
say .lua~box("boolean", "yes")
e2: say "Error:" condition("O")~message

-- 5. Booleans: .Boolean~true and .Boolean~false are Lua's true and false, both
--    ways (.true is the string "1", which Lua takes for the number 1)
say types~call(.Boolean~true, .Boolean~of(1 > 2), .true)   -- boolean boolean integer
b = .lua~eval("1 < 2")
say b~class~id b                                   -- Boolean 1: a Rexx logical value too
if b then say "IF takes it"                        -- IF takes it
say .lua~eval("(...) and 'true' or 'false'", .lua~false)   -- false (.lua~false is .Boolean~false)
exit

::requires "lua.cls"
