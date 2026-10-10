# ooRexx/Lua — design

How the bridge is built and why. The model is
[ooRexx/Python](https://github.com/JosepMariaBlasco/oorexxpython)'s
`docs/design.md`, and, like it, BSF4ooRexx: proxies by reference,
case-insensitive names, and values converted only when they are simple. The
code comments refer to the sections of this document by name (for example
"docs/design.md, *Lifetime*").

The bridge is being turned from a spike into a prototype. Every design
question the spike raised has been decided. The sections below describe what
is implemented; *Decided, not yet implemented* lists the rest.

## Shape

Two directions, of which the first exists:

- **ooRexx → Lua** (Lua embedded in ooRexx): an ooRexx native package,
  `librexxlua.so` with `lua.cls`, gives Rexx `.lua`, `.LuaState` and
  `.LuaObject`.
- **Lua → ooRexx** (ooRexx inside Lua): a Lua C module, `require "oorexx"`,
  that creates ooRexx interpreter instances and runs Rexx code. A spike
  exists outside this repository. It runs inside LuaTeX (Lua 5.3, with
  `--shell-escape`) as well as in a standalone Lua.

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
  runs inside a Lua host (LuaTeX), `.lua` will be the host's `lua_State`,
  adopted instead of made, and nothing else will change.
- Every `.LuaObject` belongs to one state and knows it (`o~"LUA.STATE"`).
  Passing it to another state is an error ("a Lua object of another
  LuaState"), not a crash.
- `s~close` is `lua_close`: it frees everything the state held, Rexx objects
  included. Using a closed state, or any of its objects, is an error
  ("the LuaState is closed"). Closing twice is harmless.
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

`tests/rexx/threads.rex` runs several Rexx threads in one state, with
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

**Cycles.** A Lua table that holds a Rexx object that holds a proxy of that
table is a cycle that neither collector can see whole, so it is never freed.
`s~close` frees it, with everything else in the state.

`tests/rexx/lifetime.rex` checks all of this: references released both
ways, the counts right, and dropped states closed (2,000 states of 1 MB do
not grow the process by 2 GB).

## C modules

Lua C modules built the usual way (LuaRocks) are not linked against
`liblua`: they expect the `lua_*` symbols to be global in the process.
ooRexx loads `librexxlua.so` with `RTLD_LOCAL`, so the `liblua` it brings in
is not, and `require` fails with "undefined symbol: lua_setfield". Before
making its first state, the bridge reopens that `liblua` with
`RTLD_NOLOAD | RTLD_GLOBAL` (found with `dladdr`), which makes its symbols
global. `tests/rexx/cmodule.rex` requires such a module, built by
`tests/run.sh`.

## Values

Rexx → Lua, with **the same rule as ooRexx/Python** (and the same code, its
`numberKind`), so that there is only one rule to remember. A Rexx string
becomes:

- a canonical integer (`0`, `42`, `-7`: no leading zeros, blanks or `+`)
  that fits in 64 bits → a Lua integer;
- any other valid Rexx number with a decimal point or an exponent (`3.14`,
  `-0.5`, `1E3`, `.5`) → a Lua float;
- anything else (`"007"`, `" 5"`, `"+5"`, text, and integers that do not
  fit in 64 bits, which a float would round silently) → a Lua string.

Lua converts strings to numbers in arithmetic only: `"10" == 10` is false,
and `"10" < 20` and `math.max("1", 2)` fail. With strings always, a Lua
function that compares a number it got from Rexx would fail. The cost is the
same as in ooRexx/Python: a string that looks like a number arrives as a
number. `..` and `tostring` usually give it back unchanged; when they do not,
`box` forces a type (see *Boxing*). The keys of `o[k]` and `o[k] = v`
follow the same rule, so `t[1]` from Rexx is `t[1]` in Lua.

| Rexx | Lua |
|---|---|
| `.nil` | `nil` |
| `.LuaObject` | its value (of the same state) |
| Array | a new sequence table (a copy), each item by this rule |
| a string (literal numbers included) | integer, float or string, by the rule above |
| Routine | a real Lua function (a C closure) |
| any other object | a `RexxObject` userdata: `obj:name(...)` sends a message, `obj(...)` sends `CALL` |

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

`o~name(args)` gets the field and, if it is callable, calls it. With no
arguments it still calls: as in ooRexx/Python, a function value needs
`o~get(name)`. The first result is returned; `~callAll` returns them all as
an Array. Lua errors become SYNTAX 98.900 "Lua error: ...", with Lua's
traceback. A Rexx condition raised inside a callback becomes a Lua error, and
so it can come back to Rexx, nested.

## Decided, not yet implemented

Each of these changes or extends the sections above.

1. **The colon problem.** The choice between `o:name(...)` and
   `o.name(...)` will depend on the function, not on the object. A Lua
   function whose first parameter is `self` (`lua_getlocal(L, NULL, 1)`;
   every `function X:m()` has it) is called with `:`, any other with `.`.
   This is right for modules, for *Programming in Lua*'s classes
   (`Account:new`, with no metatable on the class), for objects with
   metatables and for objects made of closures. A C function, which has no
   parameter names, is called with `.` when it was found in the object
   itself (`string.rep`) and with `:` when it was found through its
   metatable's `__index` (`file:read`, `("abc"):upper()`). `~method(name,
   ...)` and `~func(name, ...)` still force one form or the other.
2. **Calling vs getting** stays as it is (`~get(name)` for a function value).
3. **Several results.** The first one by default. `~callAll(args...)` and a
   new `~all(name, args...)` (which follows the colon rule) return an Array
   with one item per result, `nil` as `.nil` in its place: no holes, `items`
   is the number of results, and `return nil` differs from `return`.
4. **Rexx objects in Lua.** `obj.name = v` sends `NAME=`; keys that are
   strings are messages, keys of any other type use Rexx's `[]` (`a[1]`,
   `ipairs(a)` on a Rexx Array); `#obj` sends `items`; `pairs(obj)` goes
   through its `supplier`. Only Arrays are copied, as in ooRexx/Python;
   `.lua~table(d)` copies a Directory.
5. **Operators on proxies**, as in ooRexx/Python: Lua's arithmetic (with
   metamethods); `=` is Lua's `==`, `==` is `rawequal`; `hashCode`
   consistent with `=`.
6. **Lua versions:** 5.3, 5.4 and 5.5 from the same source (5.3 is needed
   for LuaTeX); 5.4 by default. Not LuaJIT (Lua 5.1's API, no integers).
7. **Coroutines** are used like any function. A `yield` across a callback
   into Rexx fails with Lua's own "attempt to yield across a C-call
   boundary": to be documented, not worked around.
8. **Platforms:** Linux first; macOS and Windows, with CI as in the sister
   bridges, once the prototype is stable.
