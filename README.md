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

**Status: prototype.** It works and is tested on Linux x86_64 with
ooRexx 5.3.0 and Lua 5.3, 5.4 and 5.5. Every design question the first spike
raised is decided and implemented (`docs/design.md`); what is left is listed
there, in *Not yet done*. The API may still change.

## Getting started

You need Linux, a C++ compiler, an ooRexx 5.3.0 with its API headers (for
example, the one installed in `/usr/local`) and Lua's development files:
Lua 5.4 by default (Debian/Ubuntu: `liblua5.4-dev`), or 5.3
(`LUA_VERSION=5.3`, `liblua5.3-dev`), or 5.5 (`LUA_VERSION=5.5`, built next
to the clone by `scripts/setup-lua.sh`).

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
the small Lua C module the tests load, with the same `LUA_VERSION` as the
build. To test another Lua, build it into a directory of its own:

```bash
LUA_VERSION=5.3 BUILD=build53 oorexxlua/scripts/build.sh
LUA_VERSION=5.3 BUILD=build53 oorexxlua/tests/run.sh
```

## Layout

- `src/` — the bridge.
  - `rexxlua.cpp` — `librexxlua.so`, the ooRexx native package, with Lua
    linked in.
  - `lua.cls` — the Rexx side: `.lua`, `.LuaState`, `.LuaObject` (its
    header gives the API).
- `examples/` — short programs, each with its exact output: `01-tour.rex`
  (one feature per section), `02-states.rex` (several Luas, closing,
  identity), `03-values.rex` (how values cross, `box`), `04-calls.rex`
  (the colon rule, several results, coroutines), `05-rexx-in-lua.rex`
  (Rexx objects used from Lua), `06-operators.rex`; `run-one.sh`.
- `tests/` — `run.sh`; `rexx/` (lifetime, threads, C modules, and
  `robustness.rex`, a test for every fault a review found), `cmodule/`
  (a Lua C module built as LuaRocks builds them).
- `scripts/` — `build.sh` (the build) and `setup-lua.sh` (builds a Lua
  from lua.org, for 5.5).
- `docs/design.md` — how the bridge works and why, and what is not yet
  done.

## Credits

Like its sisters, this bridge follows BSF4ooRexx, the ooRexx–Java bridge by
Rony G. Flatscher.

Written by Josep Maria Blasco, with Claude (Anthropic) as coding assistant.

## License

Apache License 2.0 (`LICENSE`).
