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

**Status: prototype.** It works and is tested, by the CI on every push, on
Linux x64, macOS (arm64 and x64) and Windows x64, with ooRexx 5.3.0 and Lua
5.3, 5.4 and 5.5. Every design question the first spike
raised is decided and implemented (`docs/design.md`); what is left is listed
there, in *Not yet done*. The API may still change.

## Binaries

Each [release](https://github.com/JosepMariaBlasco/oorexxlua/releases) has a
zip for each platform (`linux-x64`, `macos-arm64`, `macos-x64`,
`windows-x64`) and Lua (`lua5.3`, `lua5.4`, `lua5.5`): twelve. Each carries
its Lua inside, so no Lua needs to be installed; pick the version your Lua
code and C modules are made for (5.4 if unsure). Its `README.md` says how to
install it, and `check.rex` checks the installation. Every push to `main`
also makes them, as artifacts of the CI run, without a release.

## Getting started

You need a C++ compiler (on Windows, MSVC, used from Git Bash in a developer
command prompt's environment), an ooRexx 5.3.0 with its API headers (the one
installed in `/usr/local`, say, or a portable build) on the `PATH`, and a
Lua: 5.4 by default, or `LUA_VERSION=5.3` or `5.5`.

- On Linux, the distribution's development files (Debian/Ubuntu:
  `liblua5.4-dev`, `liblua5.3-dev`), or a Lua built next to the clone by
  `scripts/setup-lua.sh` (5.5, which no distribution packages yet).
- On macOS and Windows, a Lua built next to the clone by
  `scripts/setup-lua.sh 5.4` (or `5.3`, `5.5`): a static library on macOS,
  and on Windows `lua54.dll`, which the bridge and Lua's C modules share.

```bash
git clone https://github.com/JosepMariaBlasco/oorexxlua
oorexxlua/scripts/setup-lua.sh 5.4           # not needed with liblua5.4-dev
oorexxlua/scripts/build.sh                   # librexxlua and lua.cls, seconds
oorexxlua/examples/run-one.sh 01-tour.rex
```

The build goes into `build/`, next to the clone, in the directory that
contains it (`$ORXLUA_WORK`, which you can override; `BUILD` overrides the
directory itself): `librexxlua.so` (`librexxlua.dylib` on macOS;
`rexxlua.dll` on Windows, with `lua54.dll`) and `lua.cls`. To use it from
your own programs, put the build directory on `REXX_PATH` and on the library
path (`LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH` on macOS, `PATH` on Windows):
`LD_LIBRARY_PATH=build REXX_PATH=build rexx prog.rex`.

## Tests

```bash
oorexxlua/scripts/setup-oorexxunit.sh        # ooRexx's test framework, once
oorexxlua/tests/run.sh
```

The tests are ooRexxUnit test groups (`tests/rexx/*.testGroup`), each run in
its own process under a timeout (a deadlock is a failure).
`Examples.testGroup` runs every example and compares its output with the
expected one (`NAME.out` next to it). `TESTS="Threads.testGroup"` chooses.
`run.sh` also builds the small Lua C module the tests load, with the same
`LUA_VERSION` as the build. To test another Lua, build it into a directory
of its own:

```bash
LUA_VERSION=5.3 BUILD=build53 oorexxlua/scripts/build.sh
LUA_VERSION=5.3 BUILD=build53 oorexxlua/tests/run.sh
```

The CI (`.github/workflows/build.yml`) does the same on the four platforms,
with the three Luas.

## Layout

- `src/` — the bridge.
  - `rexxlua.cpp` — `librexxlua`, the ooRexx native package, with Lua
    linked in.
  - `lua.cls` — the Rexx side: `.lua`, `.LuaState`, `.LuaObject` (its
    header gives the API).
- `examples/` — short programs, each with its exact output: `01-tour.rex`
  (one feature per section), `02-states.rex` (several Luas, closing,
  identity), `03-values.rex` (how values cross, `box`), `04-calls.rex`
  (the colon rule, several results, coroutines), `05-rexx-in-lua.rex`
  (Rexx objects used from Lua), `06-operators.rex`; `run-one.sh`.
- `tests/` — `run.sh`; `rexx/`, the ooRexxUnit test groups (examples,
  lifetime, threads, C modules, and `Robustness`, a test for every fault a
  review found) with their base class `LuaTestCase.cls`; `cmodule/` (a Lua
  C module built as LuaRocks builds them).
- `scripts/` — `build.sh` (the build), `platform.sh` (what differs between
  Linux, macOS and Windows), `setup-lua.sh` (builds a Lua from lua.org),
  `build-cmodule.sh` (the test C module), `package.sh` (the zip of
  binaries), `setup-oorexxunit.sh` (fetches ooRexxUnit) and
  `setup-oorexx.sh` (builds ooRexx from its sources, on Linux).
- `dist/` — what goes into the zip besides the binaries: its `README.md`
  and `check.rex`.
- `.github/workflows/build.yml` — the CI, the packages and the releases.
- `docs/design.md` — how the bridge works and why, and what is not yet
  done.

## Credits

Like its sisters, this bridge follows BSF4ooRexx, the ooRexx–Java bridge by
Rony G. Flatscher.

Written by Josep Maria Blasco, with Claude (Anthropic) as coding assistant.

## License

Apache License 2.0 (`LICENSE`).
