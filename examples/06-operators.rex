/* 06-operators.rex -- Rexx operators on Lua objects are Lua's.
   Its exact output is 06-operators.out.  Run: examples/run-one.sh 06-operators.rex */

-- 1. Metamethods: a Lua vector type, used with Rexx's operators
.lua~eval("V = {}; V.__index = V;" -
  "function V.new(x, y) return setmetatable({x = x, y = y}, V) end;" -
  "V.__add = function(a, b) return V.new(a.x + b.x, a.y + b.y) end;" -
  "V.__mul = function(a, k) return V.new(a.x * k, a.y * k) end;" -
  "V.__unm = function(a) return V.new(-a.x, -a.y) end;" -
  "V.__eq = function(a, b) return a.x == b.x and a.y == b.y end;" -
  "V.__lt = function(a, b) return a.x^2 + a.y^2 < b.x^2 + b.y^2 end;" -
  "V.__tostring = function(a) return '(' .. a.x .. ', ' .. a.y .. ')' end")
V = .lua~global("V")
a = V~new(1, 2); b = V~new(3, 4)
say a + b                                          -- (4, 6)
say a * 3                                          -- (3, 6)
say -a                                             -- (-1, -2)
say (a < b) (b < a)                                -- 1 0

-- 2. = is Lua's == (with __eq); == is identity (rawequal)
c = V~new(1, 2)
say (a = c) (a \= c)                               -- 1 0: equal values
say (a == c) (a \== c)                             -- 0 1: different tables
say a == .lua~eval("...", a)                       -- 1: the same table, another proxy

-- 3. So Lua objects work as keys of Rexx collections
seen = .table~new
seen[a] = "first"
say seen[.lua~eval("...", a)] seen~hasIndex(c)     -- first 0

-- 4. Plain numbers go through box, with Lua's arithmetic
seven = .lua~box("integer", 7)
say (seven + 1) (seven / 2) (seven % 2) (seven // 2) (seven ** 2)   -- 8 3.5 3 1 49.0
say (-7 // 2) (.lua~box("integer", -7) // 2)       -- -1 1: Rexx's remainder vs Lua's modulo
say (seven & 3) (seven | 8) (seven && 1)           -- 3 15 6: bitwise
say \.lua~box("integer", 0)                        -- -1: Lua's ~0

-- 5. An operator a Lua value does not support is a Lua error
signal on syntax name e1
say .lua~table + 1
e1: say "Rexx catches:" condition("O")~message
exit

::requires "lua.cls"
