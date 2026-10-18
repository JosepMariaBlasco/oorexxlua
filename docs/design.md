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
- a Rexx number with a decimal point or an exponent that a Lua float
  writes back unchanged (`3.14`, `-0.5`, `100.0`, `0.333333333`, `-0.0`,
  `1E+17`, `1.2676506E+30` from `2**100`) → a Lua float. "Writes back" is
  the Lua → Rexx rule below, so such a number makes the round trip
  unchanged;
- anything else → a Lua string: `"007"`, `" 5"`, `"+5"`, text; integers
  that do not fit in 64 bits, which a float would round silently; and the
  Rexx numbers a float would write differently, `3.10` (from
  `format(x, , 2)`), `1E3`, `1e+17`, `.5`, numbers with more digits than a
  float keeps (`2**100` under `numeric digits 30`); `nan`, `+infinity` and
  `-infinity`, which are not Rexx numbers (`box("float")` reads them).

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
| `.Boolean~true`, `.Boolean~false` (`isA(.Boolean)`: subclasses too) | `true`, `false` |
| `.LuaObject` | its value (of the same state) |
| Array | a new sequence table (a copy), each item by this rule; `n` = its size if it has empty slots; more dimensions: nested tables (below) |
| a string (literal numbers included) | integer, float or string, by the rule above |
| Routine | a real Lua function (a C closure) |
| any other object | a `RexxObject` userdata, by reference (see *Rexx objects in Lua*) |

Lua → Rexx:

| Lua | Rexx |
|---|---|
| `nil` | `.nil` |
| boolean | `.Boolean~true` / `.Boolean~false` |
| integer | its decimal string |
| float | the shortest digits that give back the same number, written as ooRexx writes `x + 0` under `numeric digits 17`, with `.0` if it is an integer written without an exponent; `nan`, `+infinity`, `-infinity` |
| string | a string, bytes unchanged |
| a Rexx object passed in earlier | that same object |
| table, function, userdata, thread | a `.LuaObject` proxy |

Floats are written by the rule the ooRexx bridges share (oorexx-bridges,
`topics/float-text.md`, decided 2026-10-18):

- **The digits**: the shortest that give back the same double:
  `0.30000000000000004`, `0.3333333333333333`, `5E-324`. Lua's own
  `tostring` (`%.14g`) would lose digits (`0.3`), and a float that went from
  Lua to Rexx and back would no longer be the same number.
- **The exponent** where Rexx puts it, under `numeric digits 17`: plain
  digits unless the integer part needs more than 17 places or the fraction
  more than 34; else one digit, the point, the rest, `E`, the sign and the
  exponent. `1e16` is `10000000000000000.0`, `1e17` is `1E+17`, `1e-7` is
  `0.0000001`, `1e-35` is `1E-35`. Rexx's own `x + 0`, under `numeric
  digits 17`, gives the same text (the tests check it on 20,000 doubles).
- **The `.0`**: Lua tells integers from floats by the text, so this bridge
  is on the `.0` side of the shared rule. A float whose value is an integer,
  written without an exponent, ends in `.0` (`2^10` is `1024.0`, `-0.0`
  keeps its sign), so that it comes back as a float. Integers never have
  one. In Rexx only `==` sees the difference (`1.0 = 1`), and Rexx's
  arithmetic keeps it (`1.0 + 1` is `2.0`).
- **NaN and the infinities** are `nan`, `+infinity` and `-infinity`,
  ooRexx's own spelling: what it writes for a `double` an extension hands
  it, and the only spelling its `ObjectToDouble` reads. They are not Rexx
  numbers, so Rexx arithmetic on them is an error. Going back, the rule
  above takes Rexx numbers only, so they reach Lua as strings; the way back
  to a float is `box("float", x)` (the family's decision, 2026-10-18), which
  reads these three and no other spelling (not Lua's `inf`, not `NaN` or
  `Infinity`).

**Booleans.** In Rexx, `.true` and `.false` are the strings `"1"` and `"0"`,
which nothing can tell from the numbers. `.Boolean` is the class the ooRexx
bridges share for this (the user's decision of 2026-10-17, proposed for the
ooRexx core in RFE #880): two instances, `.Boolean~true` and
`.Boolean~false`, that behave as `"1"` and `"0"` in every Rexx context
(`IF`, operators, `DATATYPE`, string methods, collection indexes) and answer
`isA(.Boolean)`. Any object that is `isA(.Boolean)` goes to Lua as a
boolean, and Lua's booleans come to Rexx as those two instances, so a Lua
boolean that goes through Rexx and back is still a boolean. `.true`, a
string, still goes as the integer `1`. `src/boolean.cls` is a verbatim copy
of `oorexx-bridges`' `shared/boolean/boolean.cls`, **version 2** (SHA-256
`080fd7e21beb...`), never changed here; `lua.cls` requires it, so a program
that requires `lua.cls` sees `.Boolean`, and it ships next to `lua.cls`.
Its prolog also puts the class in `.environment` as `BOOLEAN` (unless an
entry is there already: a core class, or the copy another package loaded
first), so code that requires nothing sees it too, as it would see a core
class. When the class reaches the ooRexx core, the copy goes. `lua.cls` puts
the class in `.local` (`LUA.BOOLEAN`), where the bridge finds it from any
interpreter instance: calls from Lua run in no package that could name it.
It is one class for the whole process (ooRexx shares a package loaded from
one file among the instances; measured with two instances of the Lua
module, `oorexx-bridges` `topics/booleans.md`). `tests/rexx/boolean-test.rex` is the class's own tests,
verbatim too, run by `Booleans.testGroup`.

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
`s~table(d)` copies a Directory or a StringTable to a table, with the keys
as stored: `d~name = "x"` stores `NAME`, `d["name"]` keeps the case. A
subclass of Directory, StringTable or Array is copied as its class is, read
as ooRexx itself enumerates it, by `SUPPLIER` (what `DO WITH INDEX k ITEM v
OVER x` sees): an own `SUPPLIER` is honoured, an own `AT` only as far as
`SUPPLIER` uses it, and what the subclass only computes (`UNKNOWN`) is not
data and is not copied. An Array subclass also gives its shape by
`DIMENSIONS`, so trailing empty slots and every dimension are kept, as for
the class (`oorexx-bridges`, `topics/directory-crossing.md`, 2026-10-18,
with its amendment to point 2).

The copy keeps the Array's shape (`oorexx-bridges`, `topics/array-shape.md`,
decided 17/10/2026; until then a sparse Array lost its size and a
multi-dimensional one was flattened):

- **Empty slots.** An empty slot, or an item `.nil`, is `nil` in Lua, a hole,
  and with holes `#t` cannot give the size. So such a table also gets the
  field `n` with the Array's `size`, Lua's own convention (`table.pack`,
  `table.unpack(t, 1, t.n)`): `[1, , "x", ]` (size 4) is
  `{1, nil, "x", n = 4}`. An Array without holes is a plain sequence, with
  no `n`: its size is `#t`. In Lua, the size is `t.n or #t`. The size is
  Rexx's: `.array~new(100)` with one item appended is `{1, n = 100}`.
- **Several dimensions.** Nested tables, the first dimension outermost: a
  2×3 Array `m` is `{{m[1,1], m[1,2], m[1,3]}, {m[2,1], m[2,2], m[2,3]}}`,
  and each innermost table follows the rule above (`n` if it has holes;
  the outer ones never have holes). The way back is not symmetric: in Lua a
  table is a `.LuaObject`, and `~makeArray` of a nested one gives an Array
  of `.LuaObject`s.
- **One table per Array.** An Array that appears twice in what is converted
  is one table, and an Array that contains itself (`a~append(a)`) is a table
  that contains itself: `t[3] == t` in Lua, as `a[3] == a` in Rexx (until
  17/10/2026 that recursed in C until the process crashed). Arrays nested
  more than 1000 deep are Error 98.900 ("an Array nested more than 1000
  deep"): the copy recurses in C.
- **`~makeArray`** of a `.LuaObject` takes the items `1` to `t.n` when the
  table has an integer `n` (raw), else to `#t`, and leaves a `nil` as an
  empty slot (until 17/10/2026: `.nil`), so a sparse Array that goes to Lua
  and comes back with `~makeArray` has its size and its items again. More
  than 100,000,000 items (an `n` or a `__len` that large) is Error 98.900,
  not an attempt to allocate them.

## Boxing

`s~box(type, value)` makes a Lua value of a given type, as a `.LuaObject`,
when the rule of *Values* does not give the type needed:

| type | value | Lua |
|---|---|---|
| `"boolean"` | `1` or `0` | `true` / `false` |
| `"integer"` | a whole number that fits in 64 bits (`"007"` too) | an integer |
| `"float"` | a Rexx number that a double can hold; `nan`, `+infinity`, `-infinity` | a float |
| `"string"` | anything | its string value |
| `"reference"` | a Rexx object that is not a string | the object by reference (an Array that Lua can change) |

A float keeps about 17 digits, so `box("float")` rounds the rest
(`3.14159265358979323846264` is `3.141592653589793`). But a Rexx number out
of a double's range is an error, not an infinity or a zero (the family's
rule, oorexx-bridges `topics/explicit-float-range.md`, 2026-10-18):
`1E+999` and `1.8E+308` are "too large", `1E-999` and `-1E-999` "too
small", each a SYNTAX 98.900 whose message names the number and the cause
(`box: 1E-999 is out of range for a float (too small: it would be zero)`).
A subnormal is fine (`5E-324`), and so is a Rexx zero (`0E-999`). An
infinity is asked for by name, `+infinity` or `-infinity`. Only Rexx
strings are checked: ooRexx's `ObjectToDouble`, which reads them, says
nothing when a string is out of range, so the bridge checks after it.

Rexx's `1` and `0` are both true in Lua, and `.true` is just the string
`"1"`, so they cannot be converted automatically: a boolean is a `.Boolean`
(`.Boolean~true`, `.Boolean~of(a > b)`; *Values*). `s~true` and `s~false`
are aliases of `.Boolean~true` and `.Boolean~false` (until 0.3.0, they were
`.LuaObject`s of Lua's booleans, `s~box("boolean", ...)`, which still makes
one). For the results of Rexx callbacks that Lua uses as conditions
(`table.sort`'s comparator), a routine can return a `.Boolean`, or
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

The ooRexx bridges share these rules (the user's decision of 2026-10-16,
for the four of them: ooRexx/Python, ooRexx/.NET, ooRexx/WASM and this one).

**A Lua error in Rexx** is SYNTAX 98.900, with `condition("O")~additional`
as in BSF4ooRexx:

| `additional` | |
|---|---|
| `[1]` | `"Lua error: "` and the error's text (`tostring`, so `__tostring` too): the condition's message |
| `[2]` | the error value: a string as it is; the Rexx object, if Lua raised one it holds; any other value (a table, a number, a boolean) a `.LuaObject`. `error()` (nil) leaves it out |
| `[3]` | Lua's traceback (`"stack traceback:..."`), when there is one (not when out of memory) |

So `error({code = 42})` arrives with `condition("O")~additional[2]~code`
= 42. The traceback is not in the message (until 0.3.0 it was, after a line
end). The message handler of the bridge's `lua_pcall` keeps the value,
its text and the traceback; it runs `__tostring` there, inside the protected
call, so a `__tostring` that fails is that error, not a crash. The bridge's
own errors ("the LuaState is closed"...) are 98.900 "Lua error: ..." with
`[1]` only.

**A Rexx condition in Lua** (in a callback, or in a call from the Lua
module) is a Lua error whose value is a table with the condition's fields,
named as the other bridges name them, in Lua's lowercase:

| Field | |
|---|---|
| `condition` | `"SYNTAX"` |
| `code` | `"41.1"` (a string: `41.10` is not `41.1`) |
| `rc` | `41` |
| `errortext` | `"Bad arithmetic conversion."` |
| `message` | the secondary message, `'Nonnumeric value ("a") used in arithmetic operation.'` |
| `program` | the program's name |
| `line` | the line (absent when the error was not in Rexx code: a message sent by the bridge) |
| `traceback` | the Rexx traceback, as Rexx shows it, lines joined by `"\n"` |

Its metatable, shared and protected (`getmetatable(e)` is `"RexxError"`,
which tells a Rexx error from other values), has `__tostring`, which gives the
string the error was until 0.3.0 (`"Rexx error 41.1: Nonnumeric value..."`),
and `__concat`, so `print(e)`, `tostring(e)`, `"x" .. e`, `error(e)` and
the standalone `lua`'s report work as before. `type(e) == "string"` and
string methods on `e` (`e:match`) do not: `tostring(e)` first. Messages
cross whole both ways: a NUL in one does not cut it short.

**Round trips: each error comes back as itself.**

- A Rexx condition that goes through Lua and back to Rexx (the Lua code did
  not catch it, or raised the same table again with `error(e)`) is raised
  again: the same code and substitutions (SYNTAX), or the same condition,
  description, additional and result (any other), with a new traceback,
  from where it came back. Not a 98.900 around it. The table keeps its
  condition object (a weak-keyed table in the registry: the table, the
  object's userdata), and the bridge recognizes the table by its metatable.
  So a HALT that ends a callback (Error 4.1 there) gives Error 4.1 in the
  call into Lua too, and the program stops as a HALT stops it (until
  0.2.0, `~eval` and the other calls gave the 98.900; until 0.3.0 the HALT
  was recognized by its text). `error(tostring(e))` is a new Lua error.
  The table is the condition wherever it goes: a HALT's table that Lua keeps
  and raises in a later call into Lua gives Error 4.1 there too.
- A Lua error that goes through Rexx and back to Lua (the 98.900 above,
  which the Rexx code did not trap) is the error value again, not a Rexx
  error around it: the same table, the same string. A `.LuaObject` of
  another state cannot come back as itself, and is the 98.900's table.

ADDRESS LUA keeps its own rules (*ADDRESS LUA*): an error in the command is
ERROR or FAILURE, with the text and traceback as `condition('A')`, except a
HALT. A Ctrl-C while Lua code runs: *Ctrl-C*.

Getting a field, indexing, `tostring` and `#` can all run Lua code
(metamethods), and a Lua error outside a protected call does not come back
at all: Lua aborts the whole process ("PANIC: unprotected error in call to
Lua API"). So every such operation is a small C function run with
`lua_pcall` (`protect` and the `p_*` functions in `rexxlua.cpp`), and its
error is a Rexx condition like any other. Those functions use only Lua's API,
with no C++ objects: a Lua error is a `longjmp`, which would not destroy
them. Only running out of memory while pushing a value can still panic (and,
in Lua 5.3, an error in a finalizer that a garbage collection step runs
then; in a host's Lua, *Lua → ooRexx* deals with both).

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
as a Rexx condition. It is not worked around. In the states the bridge opens,
`coroutine.resume` and `coroutine.wrap` (except with Lua 5.4.0 to 5.4.3) are
the bridge's own copies of Lua's, so that a Ctrl-C stops a loop inside a
coroutine (*Ctrl-C*).

## Ctrl-C

A Ctrl-C stops Lua code as it stops Rexx code: the program gets a HALT
condition, which `signal on halt` traps, and which otherwise stops it with
Error 4.1. So do the other signals ooRexx makes a HALT of on Unix, SIGTERM
and SIGHUP, unless they were ignored when the first state was opened (as
`nohup` ignores SIGHUP): ooRexx does not halt on those either.

ooRexx's handler marks the Rexx activation that is running halted, and the
HALT comes at that activation's next clause boundary. While Lua code runs,
that activation is the one that called into Lua, and it is not running
clauses: without help, the HALT waited for the Lua code to end, possibly for
ever, and further Ctrl-Cs were ignored, since one was already pending (on
Windows the second one kills the process).

So the bridge, as `lua.c` does, puts a handler of its own before ooRexx's
(`sigaction` for SIGINT, SIGTERM and SIGHUP; `SetConsoleCtrlHandler` for
`CTRL_C_EVENT`, whose handlers run last registered first), and calls
ooRexx's from it. It marks every state whose Lua code is running
interrupted, and sets a hook (`lua_sethook`, which may be called from a
signal handler) on its main thread and on the thread the bridge last
entered or left (a coroutine that called back into Rexx), a hook that raises
a Lua error at every instruction, call and return, so that no `pcall` in the
Lua code can keep it going. Every call into Lua that returns while its state
is interrupted makes the HALT of the Rexx code that called pending itself
(`HaltThread`): if the signal came as a callback was ending, ooRexx's mark
fell on the callback's activation, which was about to go, and the HALT was
lost (1 in 25 in a stress test). A HALT already pending is not made twice.
The state stays interrupted until the outermost call into it returns: a call
from a callback (a nested `~eval`) must not let the Lua code around it go
on, even when that code catches the callback's HALT with `pcall`. Then the
main thread's hook goes; a coroutine's takes itself off when it next runs
(the coroutine may be gone by then). A call that starts with no other call
of its state running clears a mark left by a signal that came as the last
one ended (ooRexx's HALT deals with that one). No hook is set until a
signal comes: in Lua 5.4 any hook sends every instruction through the slow
path (a loop took 2.4 times as long).

The call into Lua then returns `.nil` (an empty string as `string`, an
empty Array as `makeArray` for `do over`, `.false` from a comparison), so
that the clause that called ends normally and the HALT comes at its
boundary: a call that returned no value would raise Error 91 first. A value
the clause goes on using can still raise an error before that boundary
(`x = o~field + 1` gives Error 97.1, `.nil` does not understand `+`), and
that error, not the HALT, then ends the program. ADDRESS LUA sets RC 1 with no ERROR
condition, and the HALT comes after the command. If the signal lands while a
callback's Rexx code runs, ooRexx's HALT is raised there, and the call gives
Error 4.1 (*Errors*).

The handler is installed when the first state is opened, once per process,
and only where Rexx is the host: not in an interpreter instance that the Lua
module made (*Lua → ooRexx*), where the host, LuaTeX say, has its own idea
of a Ctrl-C. Closing a state closes its `lua_State`, but the bridge's
record of it (`State` in `rexxlua.cpp`) is never freed, since proxies may
still point to it; so the handler can walk a list of all of them, which only
grows (a compare-and-swap), and what it reads of a state is atomic.

**Coroutines.** A loop inside a coroutine runs on that coroutine's Lua
thread, which the hook must be on too (`lua.c` does not stop it). So in the
states the bridge opens where Rexx is the host, `coroutine.resume` and
`coroutine.wrap` are the bridge's: Lua's own (`lcorolib.c`, of each
version), except that the coroutine being resumed is on a stack in the
state while `lua_resume` runs (nested resumes are nested C calls), and the
handler hooks every coroutine on it: the 15 outermost and the innermost, the
one running (in between, deeper than 16, a loop in a coroutine that resumes
another is not stopped). The state is found in Lua's extra space, which is
the bridge's in a state it opened and which every coroutine copies; not in
an upvalue, which Lua's functions do not have. Calling Lua's own functions
from wrappers instead, under `lua_pcall`, made a resume 2.5 times as slow
and halved how deep coroutines nest; this way neither changes, nor do the
error messages. Lua 5.4.0 to 5.4.3 cannot be told apart
(`LUA_VERSION_RELEASE_NUM` is 50400 in all four) and their `wrap` handles
an error in its coroutine in two ways: with them the bridge keeps Lua's
`wrap`, and a loop in a wrapped coroutine is not stopped (`resume` is the
bridge's). An out-of-memory error inside `lua_resume` can skip taking the
coroutine off the stack: it is emptied when the outermost call into the
state returns. A host's state (*Lua → ooRexx*) keeps Lua's functions.
`Interrupt.testGroup` tests the rest, with a Ctrl-C that the Lua code sends
itself (`interrupt()` of the test C module: `raise(SIGINT)`; on Windows
`GenerateConsoleCtrlEvent`, which reaches every process of the console, so
`tests/run.sh` runs that group in a console of its own, with `start`).

## ADDRESS LUA

`LUA` is an `ADDRESS` environment: its commands are Lua code, run in `.lua`
(whichever `.LuaState` `.local~lua` is at the time; in Rexx run from Lua,
the host's state). It is classic Rexx for what `.LuaObject` does with
objects: a line of Lua is written as it is, and Rexx builds it by
concatenation.

```rexx
address lua 'tex.sprint("hello")'          -- in LuaLaTeX
address lua
'n = 0'
'for i = 1, 10 do n = n + i end'
'n'                                        -- RESULT = 55
'return 1, nil, "m"'                       -- RESULT = an Array of 3 items
```

As `~eval`, a command is `return command` if that compiles, and the command
as a chunk otherwise, so an expression gives its value.

**RC says whether the command worked; the results go to RESULT.**

| The command | RC | Condition | RESULT |
|---|---|---|---|
| ran, no results | 0 | | dropped, as `CALL` does |
| ran, one result | 0 | | the value, converted as any (*Values*) |
| ran, several results | 0 | | an Array, as `o~all` gives (*Calls*) |
| does not compile | -1 | FAILURE | unchanged |
| no usable `.lua` (not a `.LuaState`, closed, the host's from another thread) | -2 | FAILURE | unchanged |
| a Lua error while running | 1 | ERROR | unchanged |

The sign follows ADDRESS FETCH and DOM of ooRexx/WASM: negative, the program
is wrong (FAILURE); positive, the command was right but did not succeed
(ERROR). `return nil, "message"`, Lua's way to report a failure as values,
is a success (RC 0, `result[2]` is the message). With an error,
`condition('A')` is Lua's message with its traceback, and
`condition('D')` the command, as for any command. A Rexx condition inside a
callback that the command makes is a Lua error, and so an ERROR, except a
HALT: that one is SYNTAX 4.1 after the command, which stops the program
unless trapped, as under `~eval` (a HALT condition raised by a command
handler is ignored when not trapped, so it would be lost). A Ctrl-C while
the command runs: RC 1 and no condition, then the HALT (*Ctrl-C*). A NUL in Lua's
message is kept. Commands
do not set RESULT in the language; this environment does, and
documents it.

**Registration.** An instance the Lua module makes has `LUA` from the start
(the `DIRECT_ENVIRONMENTS` option of `RexxCreateInterpreter`). Elsewhere
`lua.cls` adds it when it is loaded (`AddCommandEnvironment`, `Lua_Address`
in `rexxlua.cpp`), once per instance (marked in `.local`, even if
`LUA.ADDRESS` is called again), and never removes or replaces it: the work on
command handlers in ooRexx/WASM found that `AddCommandEnvironment` changes
the instance's table of handlers without the interpreter's lock, and that a
command sent to an environment before it is registered can cache a handler
that the garbage collector may free (both reported to ooRexx, 2026-10).
Registering before any command, from the thread that loads `lua.cls`,
avoids both. It is a redirecting handler (`REDIRECTING_ENVIRONMENTS`,
`REDIRECTING_COMMAND_ENVIRONMENT`), for `WITH`.

### WITH

```rexx
in.0 = 2; in.1 = 3; in.2 = 4
address lua 'for l in io.lines() do print(l * l) end' with input stem in. output stem out.
say out.1 out.2                                    -- 9 16
```

While a command with `WITH` runs, Lua functions of the bridge stand in, in
`.lua`'s globals, for what is redirected, and nothing else:

| Redirected | Stand-ins |
|---|---|
| `OUTPUT` | `print`, `io.stdout` |
| `ERROR` | `io.stderr` |
| `OUTPUT` or `ERROR` | `io.write`, `io.output` |
| `INPUT` | `io.stdin`, `io.read`, `io.lines`, `io.input` |
| any | `io.type`, `io.close` |

`NORMAL` is not redirected. The stand-ins of the files are tables with the
methods that make sense: `read` and `lines` for input, `write` for output
and error, and `flush` (returns the file), `setvbuf` (`true`), `close` and
`seek` (`nil` and a message) for all.

The redirection is that of the standard files, as in a process whose
standard output is redirected: **Lua's default files decide**. Where the
default output (`io.output()`) is the real `io.stdout`, and the output is
redirected, `io.write` writes to the redirection; where it is the real
`io.stderr`, and the error is, to the error's; anywhere else, Lua's
`io.write` writes to that file. `io.read` and `io.lines()` (with no file
name; with one, they are Lua's) do the same with the default input and
`io.stdin`. `io.output` and `io.input` are Lua's, with each stand-in as the
real file it stands for, both ways: `io.output()` is `io.stdout`'s stand-in
while the default output is the real `io.stdout`, `io.output("log.txt")`
makes `io.write` write to that file, as in Lua, and `io.output(io.stdout)`
or `io.output(io.stderr)` brings the redirection back. So a default output
that was a file before the command is still that file during it (`print`,
which writes to the standard output, is redirected all the same), and what
the command leaves as the default files stays after it, as without `WITH`.
The real files are the values of `io.stdout`, `io.stderr` and `io.stdin`
when the command starts: if they are not Lua's own (code that put a table
of its own there), `io.write` writes to Lua's default output. `io.close`
is Lua's with each stand-in as its real file (`nil` and "cannot close
standard file"). During a command, `io.type` says `"file"` of a stand-in,
and `"closed file"` of one whose command has ended; outside, Lua's says
`nil`. An `io` function that is not in `io` (a sandbox's) gets no stand-in.
Lua's own functions called through a stand-in raise their errors as when
called directly ("ADDRESS LUA:1: bad argument #1 to 'write'").

**Output** goes through the redirector's `WriteOutputBuffer` and
`WriteErrorBuffer`, which cut it into lines at its line ends and join a
line written in several pieces: `io.write("a"); io.write("b\n")` is one
line, `ab`, and a last line without a line end is a line too. `print` is
one write per call, so its line is never cut (in Lua 5.3, through the
global `tostring`, as 5.3's `print`). Numbers are written as that Lua's
`io.write` writes them.

**Input** is read a line at a time (`ReadInput`), when a read needs it;
each line ends in `\n` for Lua. The formats are Lua's: `"l"` (the default),
`"L"`, `"a"`, `"n"` (as Lua's own reader, a character at a time: what is
not a numeral stays; the decimal point is `.` whatever the locale), and a
count; Lua 5.1's `"*l"` too. As from a file, `nil` at the end, and after the
first format that fails, no more values; a format is checked when its turn
comes, as Lua does.

**Afterwards** each name gets its value back, unless the command changed it
(`print = myPrint` stays). With an error, or a Ctrl-C, the command's
output so far stays in its target, and the names are put back all the same
(directly, not in a protected call, which the stop hook of *Ctrl-C* would
end at once). A stand-in kept in a variable and called after its command
ended is a Lua error ("the ADDRESS LUA command this stream was redirected
for has ended"); each one carries its command's number, so in nested
commands (Rexx code called back from Lua, with an `ADDRESS LUA ... WITH` of
its own) each writes to its own command's target. A Rexx condition that the
target or source raises (a stream's `lineout` that fails, say) is a Lua
error, so the command's ERROR.

Only `.lua`'s Lua sees the redirection: code that kept `print`, `io.write`
or the real `io.stdout` in a local before the command, and code in C that
writes to the process's standard output, write where they always did, and
other `.LuaState`s are not touched. (Redirecting the real `FILE*` has no
portable way: `fopencookie` on Linux, `funopen` on macOS, nothing on
Windows short of a pipe and a thread.) Every
stand-in runs inside the command, under the state's lock (*Threads*), in
the command's thread.

**Needs ooRexx trunk r13271 or later, with the fixes in
`patches/oorexx/`**: the redirector's queries (`IsInputRedirected` ...) ran
with a flag uninitialized (ooRexx bug #2113, fixed in r13271), and the
registration fixes of #2111 and #2112 (above), not yet in trunk. The tests of `WITH`
(`AddressWith.testGroup`) run only with `ORXLUA_OOREXX_FIXES=1`: on Linux in
the CI, everywhere when it is run with `oorexx_fixes`.

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
Rexx conditions are Lua errors, tables with the condition's fields whose
`tostring` is "Rexx error 41.1: ..."; a Lua error that goes through Rexx
comes back as itself (*Errors*).

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
after, which frees the local references the call made. The instance is the
call's own (a module function's), or for a Rexx object or routine the one
whose Rexx code made its userdata, so that it runs with its own `.local`,
`.lua` and `ADDRESS` (another live one if that instance has been closed).
Every instance has its own `.lua` object for the host's state, and Lua
values reaching Rexx get the one of the instance they reach.

Between attaching and detaching, Lua can raise an error of its own (out of
memory, or in Lua 5.3, LuaTeX's, an error in some finalizer, which a
garbage collection step started by an allocation raises there), and a
`longjmp` past the detach would leave the thread attached and the instance
busy forever. So `hostRun` attaches, runs the same C function again inside
`lua_pcall` (with a marker as its first argument, which tells it that it is
that protected run), and then, whatever happened, puts the `State` back
and detaches before raising the error. `hostRun` itself holds no C++
object (*Errors*).

A Rexx object's userdata that Lua finalizes outside any Rexx code cannot let
go of its object then (no thread context): it is queued, and let go at the
next entry into Rexx.

**Nesting.** In a state the bridge makes (`.LuaState~new`, and `.lua` when
Rexx started), `package.preload.oorexx` gives the module of the instance
whose Rexx code is running: Lua code that does `require "oorexx"` there
gets that instance, with no new instance and no second copy of the library.
It has no `interpreter`. What its calls make is released at once, as in
callbacks (*Lifetime*): a loop of a million of them inside one Rexx call
stays flat.

**Adopting.** `lua.cls` makes `.lua` with `.LuaState~"LUA.DEFAULT"`, which
asks the bridge (`LUA.ADOPTNEXT`) whether the current instance is one the
module made; if so, the `.LuaState~new` that follows on that thread gets the
host's state instead of a new one. `init` takes no pointer that Rexx code
could pass.

**LuaTeX.** `lualatex --shell-escape` (LuaTeX loads no C module without it),
with the module on `package.cpath`: `\directlua{rexx = require "oorexx"}`,
and then `tex.sprint(rexx.eval(...))`, or Rexx code that drives TeX itself
through `.lua~global("tex")`. LuaTeX reports an uncaught error whose value
is not a string with no text (it does not use `__tostring`; TeX Live 2026's
LuaTeX 1.24), so a Rexx error (a table, *Errors*) not caught in `\directlua`
would show none: `examples/latex/rexx-in-latex.tex` calls through a
`rexxcall` that makes it its text. LuaTeX is Lua 5.3. On Linux and macOS, TeX
Live's `luatex` exports the `lua_*` symbols (checked: 115 on Linux, 168 in
both macOS architectures), so the module built against Lua 5.3 works. On
Windows LuaTeX's Lua is a DLL of its own, `lua53w64.dll`, and a module's
imports are bound by DLL name: `build.sh` also makes `texlive/oorexx.dll`,
the same module linked against an import library for that name (made from
the exports of `lua53.dll`: the same Lua 5.3 API). TeX Live also ships an
older `msvcp140.dll`, which LuaTeX's process loads, and a `std::mutex` built
by a recent MSVC (whose constructor became `constexpr`) crashes in `lock()`
with it: the Windows build defines `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`,
Microsoft's workaround. The tests of the module
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

- Maybe Rexx variables seen from the command's Lua code in `ADDRESS LUA`.
- A `lua_State` built with real `lua_lock`, so that threads do not wait for
  each other during callbacks (see *Threads*), if a real need appears.
