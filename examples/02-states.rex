/* 02-states.rex -- several independent Luas, closing one, identity.
   Its exact output is 02-states.out.  Run: examples/run-one.sh 02-states.rex  */

-- 1. .lua is the default state; .LuaState~new makes another, independent one
.lua~eval("x = 'default'")
other = .LuaState~new
other~eval("x = 'other'")
say .lua~global("x") other~global("x")             -- default other
t = other~table(.array~of(1, 2, 3))
say t~"LUA.STATE" == other                         -- 1: each object knows its state

-- 2. A Lua object cannot go to another state
signal on syntax name e1
say .lua~global("table")~concat(t, "+")
e1: say "Error:" condition("O")~message

-- 3. A Rexx object keeps its identity in Lua, and comes back as itself
d = .directory~new
same = .lua~eval("local a, b = ...; return rawequal(a, b)", d, d)
say same                                           -- 1
.lua~eval("seen = {}; seen[...] = 'here'", d)
say .lua~eval("return seen[...]", d)               -- here: usable as a key
say .lua~eval("return ...", d) == d                -- 1

-- 4. Closing a state frees everything it held; its objects are dead
say other~isClosed                                 -- 0
other~close
say other~isClosed                                 -- 1
signal on syntax name e2
say t[1]
e2: say "Error:" condition("O")~message
signal on syntax name e3
say other~eval("1 + 1")
e3: say "Error:" condition("O")~message
other~close                                        -- closing twice is harmless
exit

::requires "lua.cls"
