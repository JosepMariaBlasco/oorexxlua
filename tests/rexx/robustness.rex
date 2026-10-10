/* robustness.rex -- what a review of the bridge found (each was a crash, a
   wrong value or unbounded memory before it was fixed)                     */

-- 1. Callbacks from a coroutine use the coroutine's stack (*Coroutines*)
add = .routine~new("add", "use arg a, b; return a + b")
co = .lua~eval("coroutine.wrap(function(f) return f(1, 2) end)")
say "callback in a coroutine:" co~call(add)                        -- 3
co = .lua~eval("coroutine.wrap(function(f) local s = 0 for i = 1, 100000 do s = s + f(i, 0) end return s end)")
say "100000 of them:" co~call(add)                                -- 5000050000
d = .directory~new; d~x = 5
co = .lua~eval("coroutine.wrap(function(d) local r = {} for k, v in pairs(d) do r[#r+1] = k .. '=' .. v end return table.concat(r) end)")
say "pairs in a coroutine:" co~call(d)                            -- X=5

-- 2. Many arguments, deep Arrays (Lua's stack is checked)
args = .array~new; do i = 1 to 300; args[i] = i; end
count = .lua~eval("function(...) return select('#', ...) end")
say "300 arguments:" count~sendWith("CALL", args)                 -- 300
deep = .array~of(1); do 200; deep = .array~of(deep); end
say "an Array 200 deep:" .lua~eval("local t = ... local n = 0 while type(t) == 'table' do t = t[1] n = n + 1 end return n", deep)   -- 201

-- 3. A state cannot be closed by a callback while its Lua code runs
s = .LuaState~new
closer = .routine~new("closer", "use arg s; s~close")
signal on syntax name e1
s~eval("local f, s = ...; f(s); return 1", closer, s)
e1: say "close from a callback:" firstLine(condition("O")~message)
say "the state is still open:" (\s~isClosed) s~eval("1 + 1")      -- 1 2
s~close

-- 4. A string method that fails is an error, not a crash
signal on syntax name e2
say .lua~eval("local o = ...; return tostring(o)", .Bad~new)
e2: say "tostring of a failing object:" firstLine(condition("O")~message)
signal on syntax name e3
say .lua~box("string", .Bad~new)
e3: say "box of a failing object:" firstLine(condition("O")~message)

-- 5. A Rexx object that a Lua finalizer brought back is refused, not used
.lua~eval("local b = ...; setmetatable({box = b}, {__gc = function(t) saved = t.box end})", .directory~new)
.lua~eval("collectgarbage(); collectgarbage()")
signal on syntax name e4
say .lua~eval("tostring(saved)")
e4: say "finalized object:" firstLine(condition("O")~message)
.lua~eval("saved = nil")

-- 6. hashCode agrees with == for numbers (1 and 1.0 are rawequal)
one = .lua~box("integer", 1); onef = .lua~box("float", 1)
say "1 == 1.0:" (one == onef) (one~hashCode == onef~hashCode)     -- 1 1

-- 7. A million callbacks in one call do not pile up Rexx objects
before = peak()
.lua~eval("local f = ... for i = 1, 1000000 do f(i, 1) end", add)
say "memory stays flat:" (peak() - before < 50000)                -- KB
exit

firstLine: return arg(1)~makeArray("0a"x)[1]

peak: procedure              -- the peak resident memory of this process, in KB
  lines = .stream~new("/proc/self/status")~arrayIn
  do line over lines
    parse value translate(line, " ", "09"x) with key ":" value .
    if key = "VmHWM" then return value
  end
  raise syntax 98.900 array ("no VmHWM in /proc/self/status")

::requires "lua.cls"

::class Bad
::method string
  raise syntax 40.1 array ("Bad~string")
