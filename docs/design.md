# ooRexx/Lua — design

How the bridge is built and why, and what is still undecided. For now this
document describes a spike: the current code works, but it was written to
find the questions, not to answer them. The model is
[ooRexx/Python](https://github.com/JosepMariaBlasco/oorexxpython)'s
`docs/design.md`, and, like it, BSF4ooRexx: proxies by reference,
case-insensitive names, and values converted only when they are simple.

## Shape

Two directions, of which the first exists:

- **ooRexx → Lua** (Lua embedded in ooRexx): an ooRexx native package,
  `librexxlua.so` with `lua.cls`, gives Rexx `.lua` and `.LuaObject` proxies.
- **Lua → ooRexx** (ooRexx inside Lua): a Lua C module, `require "oorexx"`,
  that creates ooRexx interpreter instances and runs Rexx code. A spike
  exists outside this repository. It runs inside LuaTeX (Lua 5.3, with
  `--shell-escape`) as well as in a standalone Lua.

Only public APIs are used on both sides: Lua's C API and ooRexx's native API
(`oorexxapi.h`). Neither interpreter needs any change.

When both directions share a process (Rexx running inside LuaTeX that calls
back into Lua), the Rexx side must **adopt the host's `lua_State`** instead of
creating one, as ooRexx/Python does when Python is already in the process.

## Values

Lua → Rexx:

| Lua | Rexx |
|---|---|
| `nil` | `.nil` |
| boolean | `1` / `0` |
| integer | its decimal string |
| float | Lua's own `tostring` (`2^10` → `1024.0`) |
| string | a string, bytes unchanged |
| a Rexx object passed in earlier | that same object |
| table, function, userdata, thread | a `.LuaObject` proxy |

Rexx → Lua:

| Rexx | Lua |
|---|---|
| `.nil` | `nil` |
| `.LuaObject` | its value |
| Array | a new sequence table (a copy) |
| String (literal numbers included) | a string (Lua converts strings to numbers in arithmetic) |
| Routine | a real Lua function (a C closure) |
| any other object | a `RexxObject` userdata: `obj:name(...)` sends a message, `obj(...)` sends `CALL` |

The keys of `o[k]` and `o[k] = v` follow the canonical-integer rule instead:
`"2"` becomes the integer `2`, so that `t[2]` from Rexx is `t[2]` in Lua.

## Names

Rexx uppercases message names. `o~name` looks for `name` in lowercase, then
as written, then (in tables only) scans the keys without regard to case.
`o~name = v` keeps the case of an existing key, and otherwise uses
lowercase.

## Calls

`o~name(args)` gets the field and, if it is callable, calls it. With no
arguments it still calls: as in ooRexx/Python, a function value needs
`o~get(name)`. The first result is returned; `~callAll` returns them all as
an Array. Lua errors become SYNTAX 98.900 "Lua error: ...", with Lua's
traceback. A Rexx condition raised inside a callback becomes a Lua error, and
so it can come back to Rexx, nested.

## Open questions

What the spike found, and the real bridge must decide:

1. **The colon problem.** `o~name(args)` is `o:name(...)` if `o` has a
   metatable and `o.name(...)` otherwise. That is right for modules (no
   metatable) and for most objects (metatable), but it is a heuristic: Lua
   strings have a metatable too, and so do many module tables. `~method(name,
   ...)` and `~func(name, ...)` force one form or the other.
2. **Calling vs getting.** Should `o~name` without arguments call a
   function, or return it?
3. **Several results.** Is the first result by default, plus `~callAll`,
   the right default?
4. **Booleans.** Rexx's `1` and `0` are both true in Lua. `.lua~true` and
   `.lua~false` handle values going in; `.lua~predicate(routine)` handles
   the results of callbacks coming back (without it, `table.sort` with a Rexx
   comparator fails with "invalid order function"). Is there a better rule?
5. **Functions must be functions.** `table.sort` rejects a callable
   userdata, so Rexx Routines go to Lua as C closures. Other Rexx objects
   go as userdata whose `__index` sends a message (`d:at('NAME')`).
6. **Integers.** Literal Rexx numbers arrive as Integer objects, not as
   Strings: anything that is a String goes to Lua as a string. Keys follow
   the canonical-integer rule; values do not, and Lua coerces them.
7. **Floats.** Lua 5.4 formats `2^10` as `1024.0`: the float and integer
   subtypes are visible from Rexx.
8. **Class lookup.** `.lua` cannot be both a public class name and the
   `.local` entry, because `.LUA` finds the class first.
9. **Lifetime.** A proxy holds a registry reference, released in `uninit`.
   A Rexx object inside Lua holds a global reference, released by `__gc`.
   Cycles that cross the two collectors are not handled.

Not touched yet: threads (a `lua_State` is not thread-safe: it needs one lock
per state, released around every call into Rexx), coroutines, Lua 5.3,
LuaJIT and 5.5, operators on proxies, and platforms other than Linux.
