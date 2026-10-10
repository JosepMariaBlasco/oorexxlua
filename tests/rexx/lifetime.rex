/* lifetime.rex -- objects are freed on both sides (docs/design.md, *Lifetime*) */
n = 100000

-- Lua objects held by Rexx: their registry references are released
do i = 1 to n
  t = .lua~table(.array~of(i))
end
drop t
call churn
parse value .lua~"LUA.STATS" with refs .
say "Lua objects dropped by Rexx are released:" (refs < n / 2)

-- Rexx objects held by Lua: released by __gc, and only then
.lua~eval("keepme = ...", .directory~new)
do i = 1 to n
  .lua~eval("local x = ...", .directory~new)
end
.lua~eval("collectgarbage()")
parse value .lua~"LUA.STATS" with . kept
say "Rexx objects dropped by Lua are released:" (kept = 1)
say "and the one Lua still holds is alive:" (.lua~global("keepme")~isA(.Directory))

-- A Rexx object that Lua drops and gets again is counted right
d = .directory~new
do i = 1 to 1000
  .lua~eval("local x = ...; if ... then collectgarbage() end", d)
end
.lua~eval("keepme = nil; collectgarbage()")
parse value .lua~"LUA.STATS" with . kept
say "nothing left:" (kept = 0)

-- A state Rexx drops is closed by the bridge: 2000 states of 1 MB each
-- would be 2 GB if they were not
before = rss()
do i = 1 to 2000
  s = .LuaState~new
  s~eval("big = string.rep('x', 1000000)")
  if i // 100 = 0 then call churn
end
drop s
call churn
say "dropped states are closed:" (rss() - before < 500000)   -- KB
exit

rss: procedure              -- resident memory of this process, in KB
  f = .stream~new("/proc/self/status")       -- (~lines is 0 on /proc files)
  lines = f~arrayIn; f~close
  do line over lines
    parse value translate(line, " ", "09"x) with key ":" value .
    if key = "VmRSS" then return value
  end
  raise syntax 98.900 array ("no VmRSS in /proc/self/status")

churn: procedure            -- make garbage on the Rexx side, so that uninits run
  do i = 1 to 200000; a = .array~new(10); end
  x = .lua~eval("1")          -- the next native call drains the graveyard
  return

::requires "lua.cls"
