# ooRexx/Lua

A native bridge between [ooRexx](https://sourceforge.net/projects/oorexx/)
and [Lua](https://www.lua.org/), modelled after BSF4ooRexx, and sister of
[ooRexx/Python](https://github.com/JosepMariaBlasco/oorexxpython) and
[ooRexx/.NET](https://github.com/JosepMariaBlasco/oorexxnet).

The first piece is **Lua embedded in ooRexx**: an ooRexx native package,
`lua.cls`, that starts a Lua 5.4 inside the Rexx process and gives Rexx
`.lua` and `.LuaObject` proxies. Lua tables, functions and objects are used
as Rexx objects. Rexx routines and objects are passed to Lua as functions and
userdata that Lua can call back. Errors cross in both directions, nested.

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

**Status: spike.** It works and is tested on Linux x86_64 with ooRexx 5.3.0
and Lua 5.4.6. It is not yet a design: there is one `lua_State` per process,
no threads and no locks. The API will change. `docs/design.md` lists the open
questions.

## Getting started

You need Linux, a C++ compiler, an ooRexx 5.3.0 with its API headers (for
example, the one installed in `/usr/local`) and Lua 5.4's development files
(Debian/Ubuntu: `liblua5.4-dev`).

```bash
git clone https://github.com/JosepMariaBlasco/oorexxlua
oorexxlua/scripts/build.sh                   # librexxlua.so and lua.cls, seconds
oorexxlua/examples/run-one.sh tour.rex
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

This runs every example and compares its output with the expected output
(`examples/NAME.out`).

## Layout

- `src/` — the bridge.
  - `rexxlua.cpp` — `librexxlua.so`, the ooRexx native package, with Lua
    linked in.
  - `lua.cls` — the Rexx side: `.lua`, `.LuaObject` (its header gives the
    API).
- `examples/` — `tour.rex`, one feature per section, and its exact output;
  `run-one.sh`.
- `tests/run.sh` — the examples against their expected output.
- `scripts/build.sh` — the build.
- `docs/design.md` — how the bridge works, and what is still undecided.

## Credits

Like its sisters, this bridge follows BSF4ooRexx, the ooRexx–Java bridge by
Rony G. Flatscher.

Written by Josep Maria Blasco, with Claude (Anthropic) as coding assistant.

## License

Apache License 2.0 (`LICENSE`).
