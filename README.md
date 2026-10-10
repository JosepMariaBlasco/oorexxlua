# ooRexx/Lua

A native bridge between [ooRexx](https://sourceforge.net/projects/oorexx/)
and [Lua](https://www.lua.org/), modelled after BSF4ooRexx, and sister of
[ooRexx/Python](https://github.com/JosepMariaBlasco/oorexxpython) and
[ooRexx/.NET](https://github.com/JosepMariaBlasco/oorexxnet).

The first piece is **Lua embedded in ooRexx**: an ooRexx native package,
`lua.cls`, that runs Lua 5.4 inside the Rexx process. `.lua` is a ready Lua,
`.LuaState~new` makes more, independent ones, and `.LuaObject` proxies let
Rexx use Lua tables, functions and objects as Rexx objects. Rexx routines and
objects are passed to Lua as functions and userdata that Lua can call back.
Errors cross in both directions, nested. Several Rexx threads can use Lua at
once, and Lua C modules (LuaRocks's) load.

```rexx
string = .lua~global("string")
say string~rep("ab", 3, "-")                       -- ab-ab-ab

t = .lua~table(.array~of("one", "two", "three"))
say t[2] t~luaType                                 -- two table
say .lua~global("table")~concat(t, "+")            -- one+two+three

byLength = .routine~new("cmp", "return length(arg(1)) < length(arg(2))")
words = .lua~table(.array~of("ccc", "a", "bb"))
.lua~global("table")~sort(words, .lua~predicate(byLength))
say .lua~global("table")~concat(words, " ")        -- a bb ccc

::requires "lua.cls"
```

The other direction (**ooRexx inside Lua**: `require "oorexx"` from a Lua
program, LuaTeX included) will come later.

**Status: becoming a prototype.** It works and is tested on Linux x86_64
with ooRexx 5.3.0 and Lua 5.4.6. Every design question is decided, and the
decisions are being implemented: states, threads and object lifetime are
done; values and calls are next (`docs/design.md`, *Decided, not yet
implemented*). The API will change.

## Getting started

You need Linux, a C++ compiler, an ooRexx 5.3.0 with its API headers (for
example, the one installed in `/usr/local`) and Lua 5.4's development files
(Debian/Ubuntu: `liblua5.4-dev`).

```bash
git clone https://github.com/JosepMariaBlasco/oorexxlua
oorexxlua/scripts/build.sh                   # librexxlua.so and lua.cls, seconds
oorexxlua/examples/run-one.sh 01-tour.rex
```

The build goes into `build/`, next to the clone, in the directory that
contains it (`$ORXLUA_WORK`, which you can override; `BUILD` overrides the
directory itself). To use it from your own programs, put the build directory
on `REXX_PATH` and `LD_LIBRARY_PATH`:
`LD_LIBRARY_PATH=build REXX_PATH=build rexx prog.rex`.

## Tests

```bash
oorexxlua/tests/run.sh
```

This runs every example and every test, each in its own process under a
timeout (a deadlock is a failure), and compares its output with the expected
output (`NAME.out` next to it). `TESTS="threads.rex"` chooses. It also builds
the small Lua C module the tests load, so it needs `pkg-config`'s `lua5.4`
(or `LUA_CFLAGS`).

## Layout

- `src/` — the bridge.
  - `rexxlua.cpp` — `librexxlua.so`, the ooRexx native package, with Lua
    linked in.
  - `lua.cls` — the Rexx side: `.lua`, `.LuaState`, `.LuaObject` (its
    header gives the API).
- `examples/` — short programs, each with its exact output: `01-tour.rex`
  (one feature per section), `02-states.rex` (several Luas, closing,
  identity); `run-one.sh`.
- `tests/` — `run.sh`; `rexx/` (lifetime, threads, C modules), `cmodule/`
  (a Lua C module built as LuaRocks builds them).
- `scripts/build.sh` — the build.
- `docs/design.md` — how the bridge works and why, and what is decided but
  not yet implemented.

## Credits

Like its sisters, this bridge follows BSF4ooRexx, the ooRexx–Java bridge by
Rony G. Flatscher.

Written by Josep Maria Blasco, with Claude (Anthropic) as coding assistant.

## License

Apache License 2.0 (`LICENSE`).
