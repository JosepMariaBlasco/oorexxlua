# ooRexx/Lua — design

How the bridge is built and why. The model is
[ooRexx/Python](https://github.com/JosepMariaBlasco/oorexxpython)'s
`docs/design.md`, and, like it, BSF4ooRexx: proxies by reference,
case-insensitive names, and values converted only when they are simple. The
code comments refer to the sections of this document by name (for example
"docs/design.md, *Lifetime*").

The bridge is a prototype: every design question the first spikes raised
has been decided and implemented. *Not yet done* lists what is left.

## Shape

Two directions, which nest freely (Rexx calls Lua, which calls Rexx, which
calls Lua...):

- **ooRexx → Lua** (Lua embedded in ooRexx): an ooRexx native package,
  `librexxlua` (`.so`, `.dylib`; `rexxlua.dll` on Windows) with
  `lua.cls`, gives Rexx `.lua`, `.LuaState` and
  `.LuaObject`.
- **Lua → ooRexx** (ooRexx inside Lua): a Lua C module, `require "oorexx"`
  (`oorexx.so`, `oorexx.dll`), that creates ooRexx interpreter instances and
  runs Rexx code, in any Lua 5.3, 5.4 or 5.5 and in LuaTeX (with
  `--shell-escape`). See *Lua → ooRexx*.

**One source, two binaries.** Both are `src/rexxlua.cpp`. `librexxlua`
brings its Lua (linked in, or a DLL on Windows: *C modules*). The Lua module
must not: it must use the Lua that loads it, so on Linux and macOS it is the
same source built again without any Lua (its `lua_*` come from the host, as
for every Lua C module); on Windows, where a module is linked against the
Lua DLL by name anyway, it is a copy of `rexxlua.dll`. When the module makes
an interpreter instance, it registers itself with it as the library
`rexxlua` (`REGISTER_LIBRARY`), so `lua.cls`, which requires `"rexxlua"
LIBRARY`, works unchanged and no second copy of the code (or of its global
state) is ever loaded: one bridge, both directions. (ooRexx/Python needs two
libraries, a loader and the bridge, for the opposite reason: there the
bridge must not bring its Python.)

Only public APIs are used on both sides: Lua's C API and ooRexx's native API
(`oorexxapi.h`). Neither interpreter needs any change.

## States

Unlike Python, Lua can have any number of independent interpreters in one
process, and they are cheap. Each `.LuaState` is one (a `lua_State`):

```rexx
s = .LuaState~new
s~eval("x = 42")
say s~global("x") .lua~global("x")               -- 42 The NIL object
s~close
```

- `.lua` is the default state, made when `lua.cls` is loaded. When Rexx
  runs inside a Lua host (the Lua module), `.lua` is the host's `lua_State`,
  adopted instead of made, and nothing else changes (*Lua → ooRexx*).
- Every `.LuaObject` belongs to one state and knows it (`o~"LUA.STATE"`).
  Passing it to another state is an error ("a Lua object of another
  LuaState"), not a crash.
- `s~close` is `lua_close`: it frees everything the state held, Rexx objects
  included. Using a closed state, or any of its objects, is an error
  ("the LuaState is closed"). Closing twice is harmless. A state cannot be
  closed from a callback while its own Lua code is running (that Lua code
  would go on in freed memory): that is an error, and so is not done by the
  bridge either (a dropped state that is running is closed later).
- A `.LuaState` that Rexx drops is closed by the bridge (see *Lifetime*).

## Threads

A `lua_State` is not thread-safe, and the `liblua` of a distribution (or of
LuaTeX) is built without Lua's internal locks (`lua_lock` is empty). So each
state has **one recursive lock**, taken by every native method for the whole
call, **including the callbacks into Rexx** that Lua makes during it. The
same thread can enter again (Rexx → Lua → Rexx → Lua); another thread waits.

The cost is that one deadlock is possible: thread A, inside a state, calls
back into Rexx, and that Rexx code waits for thread B, which is waiting to
enter the same state. Threads that use Lua concurrently and wait for each
other should each use a state of their own (`.LuaState~new`): states share
no lock. (Releasing the lock during callbacks, as ooRexx/Python releases the
GIL, would need a Lua built with real `lua_lock`/`lua_unlock` and a Lua
thread per Rexx thread. That is kept as a possible future improvement, for
when there is a real need.)

Every method of `.LuaState` and `.LuaObject` but `init` is `UNGUARDED`, as
in ooRexx/Python: the lock above protects Lua's state, and a guarded method
would hold the Rexx object while Lua runs.

`tests/rexx/Threads.testGroup` runs several Rexx threads in one state, with
callbacks, and several threads with states of their own.

## Lifetime

The solutions are ooRexx/Python's, for the same ooRexx bugs
(oorexxpython's `notes/oorexx-bugs.md`).

**Lua objects held by Rexx.** A `.LuaObject` holds a reference in its
state's registry (`luaL_ref`). Its `uninit` is not native: it is two Rexx
clauses that only queue the pointer on `.LuaObject~graveyard`, and the
bridge releases the reference at the start of its next native method
(`drain` in `rexxlua.cpp`). A native uninit releases the kernel lock inside
ooRexx's `runUninits` loop, which crashes when other threads create objects
meanwhile (ooRexx bug #2101). The queue is one for the whole process, kept
by a global reference. A `.LuaState`'s uninit queues its pointer the same
way, and draining closes the state. A state that another thread is using is
left in the queue for later: draining never waits for a lock.

**Rexx objects held by Lua.** They are kept alive by an `IdentityTable` of
their state (object → count), not by global references:
`ReleaseGlobalReference` does not release in released ooRexx 5.x (bug
#2100). Each Rexx object has one userdata per state (a cache with weak
values in the registry), so its identity holds in Lua: `rawequal(a, b)`,
and it can be used as a table key. Its `__gc` decrements the count. The count
is needed because Lua clears a weak entry before running the old userdata's
`__gc`, so an object can get a new userdata while the old one is still
waiting for its finalizer.

**Local references.** Every Rexx object the native API returns is a local
reference of the native method in progress, freed only when that method
returns. One `eval` whose Lua code calls back into Rexx a million times
would otherwise hold a million argument arrays (650 MB were measured), so
the bridge releases what callbacks make as soon as it has been used: the
arguments, their Array, the result, the strings made from numbers, the
counts of `keep`.

One growth is ooRexx's own, not the bridge's: every Lua table or function
that reaches Rexx is a new `.LuaObject`, which has an uninit, and ooRexx
runs uninits lazily. A loop of a million callbacks that each receive a
table peaks at about 400 MB, as a plain Rexx loop that makes a million
objects with an uninit peaks at 170 MB. Callbacks that receive strings,
numbers or Rexx objects stay flat.

**Finalized objects.** A Lua finalizer (`__gc`) can bring back a userdata
whose own finalizer has already run. A Rexx object's userdata is then
refused ("a Rexx object used after Lua finalized it"), and is `.nil` if it
reaches Rexx.

**Cycles.** A Lua table that holds a Rexx object that holds a proxy of that
table is a cycle that neither collector can see whole, so it is never freed.
`s~close` frees it, with everything else in the state.

`tests/rexx/Lifetime.testGroup` checks all of this: references released both
ways, the counts right, and dropped states closed (2,000 states of 1 MB do
not grow the process by 2 GB). `tests/rexx/Robustness.testGroup` checks local
references (a million callbacks in one call leave the memory flat) and
finalized objects.

## C modules

On Linux and macOS, Lua C modules built the usual way (LuaRocks) are not
linked against `liblua`: they expect the `lua_*` symbols to be global in the
process. ooRexx loads `librexxlua` with `RTLD_LOCAL`, so the `liblua` it
brings in is not, and `require` fails with "undefined symbol:
lua_setfield". Before making its first state, the bridge reopens that
`liblua` with `RTLD_NOLOAD | RTLD_GLOBAL` (found with `dladdr`), which makes
its symbols global. That `liblua` is a shared library (a distribution's) or
`librexxlua` itself, when a static `liblua.a` was linked into it:
`build.sh` links the static library whole, so that every `lua_*` a module
may need is there, and not only those the bridge uses.

Windows has no global symbols: there, every Lua C module is linked against
the Lua DLL by name (`lua54.dll`), and the bridge must use that same DLL, or
the module would run with a second Lua of its own. So on Windows Lua is
always a DLL (`scripts/setup-lua.sh` builds it), and `build.sh` copies it
next to `rexxlua.dll`.

`tests/rexx/CModule.testGroup` requires such a module, built by
`tests/run.sh` as LuaRocks builds them on each platform.

## Values

Rexx → Lua, with **a rule that loses nothing**: a Rexx string becomes a Lua
number only when the number says everything the string said. A Rexx string
becomes:

- a canonical integer (`0`, `42`, `-7`: no leading zeros, blanks or `+`)
  that fits in 64 bits → a Lua integer;
- a Rexx number with a decimal point that a Lua float writes back
  unchanged (`3.14`, `-0.5`, `100.0`, `0.333333333`) → a Lua float. "Writes
  back" is the Lua → Rexx rule below, so such a number makes the round trip
  unchanged;
- anything else → a Lua string: `"007"`, `" 5"`, `"+5"`, text; integers
  that do not fit in 64 bits, which a float would round silently; and the
  Rexx numbers a float would write differently, `3.10` (from
  `format(x, , 2)`), `1E3`, `.5`, `1.26765060E+30` (`2**100`).

Lua converts strings to numbers in arithmetic only: `"10" == 10` is false,
and `"10" < 20` and `math.max("1", 2)` fail. With strings always, a Lua
function that compares a number it got from Rexx would fail. With numbers
whenever possible (ooRexx/Python's rule, which this one refines), Rexx's
formatting would be lost on the way: `3.10` would arrive as `3.1`, which
matters when Lua is typesetting it (LuaTeX). So a number arrives as a number
when it can do so unchanged, and as its string otherwise; Lua's arithmetic
still accepts the string. When the rule does not give the type needed,
`box` forces one (see *Boxing*). The keys of `o[k]` and `o[k] = v` follow
the same rule, so `t[1]` from Rexx is `t[1]` in Lua.

| Rexx | Lua |
|---|---|
| `.nil` | `nil` |
| `.LuaObject` | its value (of the same state) |
| Array | a new sequence table (a copy), each item by this rule |
| a string (literal numbers included) | integer, float or string, by the rule above |
| Routine | a real Lua function (a C closure) |
| any other object | a `RexxObject` userdata, by reference (see *Rexx objects in Lua*) |

Lua → Rexx:

| Lua | Rexx |
|---|---|
| `nil` | `.nil` |
| boolean | `1` / `0` |
| integer | its decimal string |
| float | the shortest of `%.15g`, `%.16g`, `%.17g` that gives back the same number, with `.0` if it looks like an integer |
| string | a string, bytes unchanged |
| a Rexx object passed in earlier | that same object |
| table, function, userdata, thread | a `.LuaObject` proxy |

Floats are written as Python's `repr` writes them: `2^10` is `1024.0`,
`0.1 + 0.2` is `0.30000000000000004`. Lua's own `tostring` (`%.14g`) would
lose digits (`0.3`), and a float that went from Lua to Rexx and back would no
longer be the same number. `inf`, `-inf` and `nan` come as those strings,
which are not Rexx numbers.

## Rexx objects in Lua

A Rexx Routine goes to Lua as a real Lua function: `table.sort` and others
check the type and reject anything else that can be called. Any other Rexx
object that is not a string or an Array goes by reference, as a userdata
(one per object and state: see *Lifetime*):

| In Lua | In Rexx |
|---|---|
| `obj:name(args)`, `obj.name(args)` | `obj~name(args)` |
| `obj.name` | the method bound to `obj` (not an attribute's value: `obj:name()`) |
| `obj.name = v` | `obj~name = v` (the message `NAME=`) |
| `obj[k]`, `obj[k] = v` with `k` not a string | `obj[k]`, `obj[k] = v` |
| `#obj` | `obj~items` |
| `pairs(obj)` | `obj~supplier`: index, item |
| `obj(args)` | `obj~call(args)` |
| `tostring(obj)` | `obj~string` |

In Lua, `obj.k` and `obj["k"]` are the same thing, so a key cannot be both a
message and an index: string keys are messages, every other key is Rexx's
`[]`. So `a[1]`, `#a` and `ipairs(a)` work on a Rexx Array, and a collection
with string keys is used as in Rexx: `d:at("NAME")`, `d:put(v, "NAME")`.
Message names go to Rexx in uppercase. A Rexx condition inside any of these
is a Lua error, which can come back to Rexx (*Errors*).

Only Arrays are copied (to a sequence table), as ooRexx/Python copies them to
lists; `s~box("reference", array)` passes one by reference instead, and
`s~table(d)` copies a Directory or a StringTable to a table.

## Boxing

`s~box(type, value)` makes a Lua value of a given type, as a `.LuaObject`,
when the rule of *Values* does not give the type needed:

| type | value | Lua |
|---|---|---|
| `"boolean"` | `1` or `0` | `true` / `false` |
| `"integer"` | a whole number that fits in 64 bits (`"007"` too) | an integer |
| `"float"` | any Rexx number | a float |
| `"string"` | anything | its string value |
| `"reference"` | a Rexx object that is not a string | the object by reference (an Array that Lua can change) |

Rexx's `1` and `0` are both true in Lua, and `.true` is just the string
`"1"`, so booleans cannot be converted automatically: `s~box("boolean",
flag)`, or the shorthands `s~true` and `s~false`. For the results of Rexx
callbacks that Lua uses as conditions (`table.sort`'s comparator),
`s~predicate(routine)` makes a Lua function that turns the routine's `1` or
`0` into a boolean. The results of other callbacks are not converted: a
routine that returns `0` as a number must not become `false`.

The argument order is that of ooRexx/Python's `.py~box` (and BSF4ooRexx's
`box`).

## Names

Rexx uppercases message names. `o~name` looks for `name` in lowercase, then
as written, then (in tables only) scans the keys without regard to case.
`o~name = v` keeps the case of an existing key, and otherwise uses
lowercase.

The Rexx package is `lua.cls`, with the default state `.lua`, the classes
`.LuaState` and `.LuaObject`, and the native library `librexxlua.so`, as
ooRexx/Python has `python.cls`, `.py`, `.Python`, `.PyObject` and
`librexxpython.so`. (`.lua` cannot be a class name: `.LUA` would find the
class before the `.local` entry.)

## Calls

`o~name(args)` gets the field (see *Names*) and, if it is callable, calls
it. With no arguments it still calls, as in ooRexx/Python: a function value
needs `o~get(name)`.

**The colon rule.** Lua has two ways to call a field, `o.name(...)` and
`o:name(...)` (which passes `o` first), and nothing in the object says which
one a function expects. The bridge looks at the function:

- A **Lua function** whose first parameter is `self` gets the object:
  `o:name(...)`. Every `function X:m()` has that parameter, implicitly. Any
  other Lua function: `o.name(...)`. The name comes from
  `lua_getlocal(L, NULL, 1)`, which Lua 5.3, 5.4 and 5.5 all have.
- A **C function** has no parameter names. Found in the object itself, it is
  a module function (`string.rep`): `o.name(...)`. Found through the
  metatable's `__index`, it is a method (a file's `read`, a string's
  `upper`): `o:name(...)`.

This is right for modules, for *Programming in Lua*'s classes
(`Account:new`, where the class itself has no metatable), for objects with
metatables, and for objects made of closures (`function o:inc()` on a plain
table), which a rule based on the object (does it have a metatable?) gets
wrong. The rule would fail only for a function written `function M.f(self,
x)` and meant to be called with a dot. `o~func(name, ...)` (`o.name(...)`)
and `o~method(name, ...)` (`o:name(...)`) force one form or the other.

**Several results.** A call returns the first result (`.nil` if none).
`o~all(name, args...)`, which follows the colon rule, and `o~callAll(args...)`
return them all, as an Array with one item per result: `nil` is `.nil` in
its place, with no holes, so `items` is the number of results, `return nil`
(one item) differs from `return` (none), and in `nil, message` (`io.open`
and friends) the message is always `r[2]`.

## Errors

Lua errors become SYNTAX 98.900 "Lua error: ...", with Lua's traceback. A
Rexx condition raised inside a callback becomes a Lua error, and so it can
come back to Rexx, nested.

Getting a field, indexing, `tostring` and `#` can all run Lua code
(metamethods), and a Lua error outside a protected call does not come back
at all: Lua aborts the whole process ("PANIC: unprotected error in call to
Lua API"). So every such operation is a small C function run with
`lua_pcall` (`protect` and the `p_*` functions in `rexxlua.cpp`), and its
error is a Rexx condition like any other. Those functions use only Lua's API,
with no C++ objects: a Lua error is a `longjmp`, which would not destroy
them. Only running out of memory while pushing a value can still panic.

**No C++ object where a Lua error is raised.** The bridge's own Lua C
functions (the callbacks into Rexx, the Lua module's functions) raise Lua
errors too, with `lua_error`. On Linux and macOS a `longjmp` destroys
nothing; MSVC's unwinds the stack as an exception does, and with `/EHsc`,
which assumes that C functions such as `lua_error` never throw, its record
of which objects are alive is not updated around such calls: it destroyed
again a `std::string` of a scope already left (heap corruption on Windows,
found with `cdb` and page heap). So no C++ object ever lives in the frame of
a function that raises: the module's functions keep theirs in `NOINLINE`
`*_body` functions, and the helpers that make C++ objects (`toRexx`,
`pushValue`, `pushRexxCondition`, `drain`...) are `NOINLINE`, so that the
compiler cannot move their objects into such a frame.

Lua guarantees only about 20 free slots on its stack, and does not check
them in release builds: the bridge makes room (`lua_checkstack`) before
pushing arguments and at every level of a nested Array, so 300 arguments or
an Array 200 deep are fine. A Rexx object whose `string` method raises a
condition (in `tostring(obj)` from Lua, or in `box`) gives that error, not a
crash.

`tests/rexx/Robustness.testGroup` holds a test for each of these, and for the
other faults that a review of the bridge found.

## Operators

Rexx operators on a `.LuaObject` are Lua's, metamethods included, as
ooRexx/Python's are Python's. The left operand must be the Lua object (Rexx
sends the message to it); the right one is converted as any value.

| Rexx | Lua |
|---|---|
| `a + b`, `a - b`, `a * b`, `a / b`, `a ** b`, `-a` | `+`, `-`, `*`, `/`, `^`, unary `-` |
| `a % b` (integer division) | `a // b` (floor division) |
| `a // b` (remainder) | `a % b` (floor modulo: `-7 % 2` is `1` in Lua, `-7 // 2` is `-1` in Rexx) |
| `a & b`, `a \| b`, `a && b`, `\a` | `&`, `\|`, `~` (bitwise: and, or, xor, not) |
| `a < b`, `a > b`, `a <= b`, `a >= b` | `<`, `>`, `<=`, `>=` (`__lt`, `__le`) |
| `a = b`, `a \= b` (`<>`, `><`) | `==`, `~=` (`__eq`) |
| `a == b`, `a \== b` | `rawequal(a, b)`: the same Lua value |
| `+a` | `a` itself |

`==` is identity, as Rexx collections expect, and `hashCode` agrees with it
(the address of a table, function, userdata or thread; the value of
anything else, a float with an integral value hashing as that integer,
since `1` and `1.0` are `rawequal`). So two proxies of one table are `==`, and Lua objects work
as keys of Rexx collections. Concatenation (`||`, abuttal, blank) is Rexx's
own, on the string value (`tostring`): a Lua table needs no `__concat` to be
shown with `say t "x"`.

## Coroutines

Lua coroutines are used from Rexx like any function: `coroutine.wrap`
gives a function, and `coroutine.resume` and friends are ordinary calls.

A callback into Rexx runs in whichever Lua thread called it: the main one,
or a coroutine. Everything the bridge does during the callback (taking its
arguments, pushing its result, and any call from that Rexx code back into
Lua) uses that thread's stack, not the main one's (`USE_THREAD` in
`rexxlua.cpp`). Using the main thread's stack there gave the callback the
wrong arguments and, in a long loop, overflowed the main stack. A coroutine still cannot yield from inside Rexx code it has
called (Lua cannot suspend the C call to Rexx that is in between): that fails
with Lua's own "attempt to yield across a C-call boundary", which comes back
as a Rexx condition. It is not worked around.

## Lua → ooRexx

```lua
local rexx = require "oorexx"
rexx.eval("6 * 7")                          --> 42 (arg(1), arg(2)... are extra arguments)
rexx.run("prog.rex", "a", "b")              --> CALL a program: its result
local f = rexx.routine("use arg a, b; return a * b")
f(6, 7)                                     --> 42: a Rexx Routine is a Lua function
local r = rexx.interpreter{rexx_path = {"/my/classes"}, extensions = ".rex",
                           initial_address = "MINE", load_libraries = "rxmath"}
r.eval("address()"); r.close()              --> another instance, with options
```

The API is ooRexx/Python's `oorexx` module, with one difference that Lua
makes natural: the module's own `eval`, `run` and `routine` work on a
**default instance**, made at their first use, so that LuaTeX documents need
no setup (`\directlua{rexx = require "oorexx"}`). `interpreter{...}` makes
others, with ooRexx's options (`EXTERNAL_CALL_PATH`, `..._EXTENSIONS`,
`INITIAL_ADDRESS_ENVIRONMENT`, `LOAD_REQUIRED_LIBRARY`), as a table with
`eval`, `run`, `routine` and `close` (dots, not colons: like the module).
Each instance has `lua.cls` loaded, found next to the module (the call path
also gets the module's directory), and the code of `eval` and `routine` sees
its public classes (`.LuaState`, `.LuaObject`), as if it required it.

Values cross by the rules of *Values* and *Rexx objects in Lua*, which were
made for both directions: results and arguments that Rexx gets from Lua are
`.LuaObject`s (tables, functions) or plain strings, and what Lua gets from
Rexx follows the rule that loses nothing (`rexx.eval("format(3.1, , 2)")` is
the string `"3.10"`), with Arrays as tables and other objects as userdata.
Rexx conditions are Lua errors ("Rexx error 41.1: ..."); Lua errors inside
them come back nested, as from Rexx.

**The host's state.** In Rexx, `.lua` is the `lua_State` of the Lua that
loaded the module (its main thread), adopted, not made: Rexx sees the host's
globals, and Lua and Rexx share every value. Everything else in *States*
holds, with three differences:

- The host closes it, not Rexx: `.lua~close` is an error. When the host
  closes its state (`lua_close`: `lua` does at the end of a script, and
  `os.exit(code, true)` anywhere), a sentinel the module put in the
  registry when it adopted the state (finalized last, after every Rexx
  object's userdata) marks the state closed, so that Rexx's proxies of its
  values give "the LuaState is closed", and terminates the module's
  instances (their Rexx uninits run then). LuaTeX does not close its state,
  and nothing needs to happen then.
- Only the host's thread may use it. The host runs Lua whenever it likes,
  outside any lock of the bridge, so another Rexx thread (`START`, `REPLY`,
  a `Message~start`) that used `.lua` would corrupt it. That is an error
  ("the LuaState of the Lua that runs ooRexx can only be used from that
  Lua's thread"), not a wait, which would deadlock as soon as the host's
  thread waits for that one. Other threads use states of their own
  (`.LuaState~new`), as *Threads* recommends anyway. The graveyard leaves the
  host's proxies to its thread.
- The `State` of a `lua_State` is in its registry (a light userdata under a
  key of the bridge), for every state: Lua's extra space, where it was,
  belongs to the host.

**Calls from the host.** Lua runs outside any Rexx code here, and calls into
Rexx when it likes: the module's functions, and also any Rexx object or
routine it holds (`d:put(v, k)`, `f(6, 7)`). Each such call attaches the
thread to an instance (`AttachThread`; the host's thread made the instances
and stays attached to them, as ooRexx bug #2107 requires) and detaches it
after, which frees the local references the call made. A Rexx object's
userdata that Lua finalizes outside any Rexx code cannot let go of its
object then (no thread context): it is queued, and let go at the next entry
into Rexx.

**Nesting.** In a state the bridge makes (`.LuaState~new`, and `.lua` when
Rexx started), `package.preload.oorexx` gives the module of the instance
whose Rexx code is running: Lua code that does `require "oorexx"` there
gets that instance, with no new instance and no second copy of the library.
It has no `interpreter`.

**LuaTeX.** `lualatex --shell-escape` (LuaTeX loads no C module without it),
with the module on `package.cpath`: `\directlua{rexx = require "oorexx"}`,
and then `tex.sprint(rexx.eval(...))`, or Rexx code that drives TeX itself
through `.lua~global("tex")`. LuaTeX is Lua 5.3. On Linux and macOS, TeX
Live's `luatex` exports the `lua_*` symbols (checked: 115 on Linux, 168 in
both macOS architectures), so the module built against Lua 5.3 works. On
Windows LuaTeX's Lua is a DLL of its own, `lua53w64.dll`, and a module's
imports are bound by DLL name: `build.sh` also makes `texlive/oorexx.dll`,
the same module linked against an import library for that name (made from
the exports of `lua53.dll`: the same Lua 5.3 API). The tests of the module
(`tests/lua`) also run in TeX Live's `texlua` (`scripts/setup-luatex.sh`
fetches it from CTAN), on the three platforms.

`tests/lua/Module.test.lua` and `tests/lua/Lifetime.test.lua` check all of
this, with any standalone Lua 5.3, 5.4 or 5.5 and with `texlua`.

## Lua versions

The same source builds with Lua 5.3, 5.4 and 5.5 (`LUA_VERSION` in
`scripts/build.sh`; 5.4 by default), and the tests give the same output with
the three. 5.3 is needed for LuaTeX; 5.5 is not packaged yet, and
`scripts/setup-lua.sh` builds it (or any of the three, on any platform)
next to the clone. The differences are a
macro (`lua_newuserdatauv` is 5.4's). All three have 64-bit integers, so the
rules of *Values* are the same.

LuaJIT is not supported: its API is Lua 5.1's, without integers, so the
rules of *Values* would not hold.

## Platforms

Linux, macOS and Windows, with the same source. What differs is in
`scripts/platform.sh`: the package's file name (`librexxlua.so`,
`librexxlua.dylib`, `rexxlua.dll`), where ooRexx finds libraries
(`LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`, `PATH`), and the compiler (MSVC on
Windows, from Git Bash). In the code, only the C modules (*C modules*)
differ.

The CI (`.github/workflows/build.yml`) builds and runs every test group on
Linux x64, macOS arm64 and x64, and Windows x64, each with Lua 5.3, 5.4 and
5.5, against ooRexx's portable builds pinned to one revision. The tests
measure memory on each platform (`/proc` on Linux, `ps` on macOS, the
working set on Windows).

The Lua 5.3 jobs also run the tests of the Lua module in TeX Live's own
LuaTeX (*Lua → ooRexx*).

**Packages.** Each of those twelve builds is also a zip of binaries
(`scripts/package.sh`), and a tag `vX.Y.Z` makes a release with them. A
package carries its Lua, so that its users need none: linked statically into
`librexxlua` on Linux and macOS (on Linux a second build, `LUA_STATIC=1`,
since the CI's first one uses the distribution's shared `liblua` for 5.3 and
5.4; `package.sh` refuses a build that needs a `liblua`), and as
`luaXY.dll` next to `rexxlua.dll` on Windows, where Lua's C modules need it
(*C modules*). Each also carries the Lua module (`oorexx.so`, `oorexx.dll`,
and on Windows with Lua 5.3 `texlive/` too). The package's `check.rex` runs
from outside the unzipped folder, finding everything as the package's
README says, and before zipping `package.sh` also has it load a C module;
`check.lua` runs the Lua module from a standalone Lua the same way. One package per Lua, not one for
all three: the bridge is compiled against one Lua, and a C module must be
built for that same version. Lua's licence (MIT) goes with each package,
taken from the `lua.h` of the Lua inside.

## Not yet done

- `ADDRESS LUA`: an `ADDRESS` environment that runs its commands as Lua
  code (`address lua 'tex.sprint("hello")'` in LuaLaTeX), as ooRexx/Python
  has Python callables as environments. Decided for later.
- A `lua_State` built with real `lua_lock`, so that threads do not wait for
  each other during callbacks (see *Threads*), if a real need appears.
