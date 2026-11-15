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
do
  say .lua~global("table")~concat(t, "+")
catch e = syntax
  say "Error:" e~message
end

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
do
  say t[1]
catch e = syntax
  say "Error:" e~message
end
do
  say other~eval("1 + 1")
catch e = syntax
  say "Error:" e~message
end
other~close                                        -- closing twice is harmless
exit

::requires "lua.cls"
