/* 04-calls.rex -- o~name(...): o:name(...) or o.name(...)? Several results.
   Its exact output is 04-calls.out.  Run: examples/run-one.sh 04-calls.rex   */

-- 1. The colon rule looks at the function: a Lua function whose first
--    parameter is `self` gets the object (every function X:m() has one)
.lua~eval("Account = {}; Account.__index = Account;" -
  "function Account:new(o) o = o or {}; setmetatable(o, self); return o end;" -
  "function Account:deposit(v) self.balance = (self.balance or 0) + v end;" -
  "function Account.describe(a) return 'balance ' .. a.balance end")
Account = .lua~global("Account")
a = Account~new                         -- Account:new(): the class is self (PiL)
a~deposit(100)                          -- a:deposit(100)
a~deposit(50)
say a~balance                           -- 150
say Account~describe(a)                 -- balance 150: no self, so Account.describe(a)

-- 2. Objects made of closures, with no metatable at all
counter = .lua~eval("function(n) local o = {n = n};" -
  "function o:inc() self.n = self.n + 1; return self.n end; return o end")~call(10)
say counter~inc counter~inc             -- 11 12

-- 3. C functions have no parameter names: found in the object itself, they
--    are module functions (string.rep); found through the metatable, they
--    are methods (a file's read, a string's upper)
say .lua~global("string")~rep("ab", 2)  -- abab
s = .lua~box("string", "hello")         -- a Lua string, as an object
say s~upper s~len                       -- HELLO 5: ("hello"):upper()
f = .lua~global("io")~tmpfile
f~write("line one", "0a"x, "line two")
f~seek("set")
say f~read("l")                         -- line one: f:read("l")
f~close

-- 4. ~method and ~func force one form or the other
say a~func("deposit", a, 25) a~balance  -- The NIL object 175: Account.deposit(a, 25)

-- 5. Several results: the first one, or all of them as an Array
string = .lua~global("string")
say string~find("hello world", "o w")   -- 5
r = string~all("find", "hello world", "o w")
say r~items r~toString(, ",")           -- 2 5,7
r = .lua~global("io")~all("open", "/no/such/file")
say r~items r[1]                        -- 3 The NIL object: nil, message, code
say r[2]                                -- /no/such/file: No such file or directory
r = .lua~eval("function() end")~callAll
say r~items                             -- 0: return nothing
r = .lua~eval("function() return nil end")~callAll
say r~items                             -- 1: return nil

-- 6. A Lua error in a metamethod is a Rexx condition, not a crash
strict = .lua~eval("setmetatable({}, {__index = function(t, k) error('no field ' .. k, 2) end})")
signal on syntax name e1
say strict~colour
e1: say "Rexx catches:" condition("O")~message~makeArray("0a"x)[1]

-- 7. Coroutines are used like any other function
gen = .lua~eval("coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i * 10) end end)")
say gen~call gen~call gen~call          -- 10 20 30
--    but a coroutine cannot yield from inside Rexx code it called
yielder = .routine~new("yielder", "return .lua~global('coroutine')~yield(1)")
co = .lua~eval("coroutine.create(function(f) return f() end)")
r = .lua~global("coroutine")~all("resume", co, yielder)
say r[1] r[2]~makeArray("0a"x)[1]       -- 0 ...attempt to yield across a C-call boundary
exit

::requires "lua.cls"
