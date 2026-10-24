# ooRexx/Lua

A native bridge between [ooRexx](https://sourceforge.net/projects/oorexx/)
and [Lua](https://www.lua.org/), modelled after BSF4ooRexx, and sister of
[ooRexx/Python](https://github.com/JosepMariaBlasco/oorexxpython) and
[ooRexx/.NET](https://github.com/JosepMariaBlasco/oorexxnet).

Both directions work. **Lua embedded in ooRexx**: an ooRexx native package,
`lua.cls`, that runs Lua 5.4 inside the Rexx process. `.lua` is a ready Lua,
`.LuaState~new` makes more, independent ones, and `.LuaObject` proxies let
Rexx use Lua tables, functions and objects as Rexx objects. Rexx routines and
objects are passed to Lua as functions and userdata that Lua can call back.
Lua's booleans are `.LogicalWrapper~true` and `~false` in Rexx, and back
(ooRexx's `logicalwrapper.cls`; JSON's booleans too). Errors cross in both
directions (a Rexx error is a table with its fields in
Lua; a Lua error keeps its value in Rexx) and come back as themselves after a
round trip, and a Ctrl-C stops Lua code with a
HALT, as it stops Rexx code. Several Rexx threads can use Lua at once, and
Lua C modules (LuaRocks's) load.

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

`ADDRESS LUA` runs commands as Lua code in `.lua`: RC says whether they
worked (ERROR or FAILURE, with Lua's message as `condition('A')`, if not),
and their results go to RESULT.

```rexx
address lua
'n = 0'
'for i = 1, 10 do n = n + i end'
'n'; say result                                    -- 55
```

With `WITH`, Lua's `print`, `io.write` and `io.read` use the redirection:

```rexx
in.0 = 2; in.1 = 3; in.2 = 4
address lua 'for l in io.lines() do print(l * l) end' with input stem in. output stem out.
say out.1 out.2                                    -- 9 16
```

Lua functions can be `ADDRESS` environments of their own: RC is the
function's value, and its context reads and writes the caller's variables
and the `WITH` redirection.

```rexx
calc = .lua~eval("return function(command, ctx) ctx:set_var('RESULT', load('return ' .. command)()) end")
.lua~addCommandEnvironment("CALC", calc)
address calc '2^10'; say rc result                 -- 0 1024.0
```

**ooRexx inside Lua**: a Lua C module, `require "oorexx"`, runs Rexx in a
Lua program (any Lua 5.3, 5.4 or 5.5, and LuaTeX, TeX Live's included). In
Rexx, `.lua` is then that same Lua, so the two sides share everything.

```lua
local rexx = require "oorexx"
print(rexx.eval("6 * 7"), rexx.eval("format(3.1, , 2)"))  -- 42  3.10
local times = rexx.routine("use arg a, b; return a * b")
print(times(6, 7))                                        -- 42
greeting = "hello"
print(rexx.eval(".lua~global('greeting')~upper"))        -- HELLO
```

In LuaLaTeX (`lualatex --shell-escape`, with the module where LuaLaTeX finds
it: `examples/latex/README.md`):

```latex
\directlua{rexx = require "oorexx"}
\newcommand\rexx[1]{\directlua{tex.sprint(rexx.eval("\luaescapestring{\detokenize{#1}}"))}}
$2^{100}$ is \rexx{2**100}, and a price: \rexx{format(3.1, , 2)} EUR.
```

A Rexx error is a Lua error whose value is a table (with the condition's
fields, and the text as `tostring`), and LuaTeX reports an error that is not a
string with no text: the example document wraps its calls in a small
`rexxcall` that turns the error into its text.

`examples/latex/` has a whole document, where a Rexx program writes a table
into the page through `ADDRESS LUA`.

**Status: prototype.** It works and is tested, by the CI on every push, on
Linux x64, macOS (arm64 and x64) and Windows x64, with ooRexx 5.3.0 and Lua
5.3, 5.4 and 5.5. Every design question the first spike
raised is decided and implemented (`docs/design.md`); what is left is listed
there, in *Not yet done*. The API may still change. The Lua module is also
tested, by the CI, in TeX Live's own LuaTeX (`texlua`) on the three
platforms.

## Binaries

Each [release](https://github.com/JosepMariaBlasco/oorexxlua/releases) has a
zip for each platform (`linux-x64`, `macos-arm64`, `macos-x64`,
`windows-x64`) and Lua (`lua5.3`, `lua5.4`, `lua5.5`): twelve. Each carries
its Lua inside, so no Lua needs to be installed; pick the version your Lua
code and C modules are made for (5.4 if unsure). Its `README.md` says how to
install it, and `check.rex` and `check.lua` check the installation (the Lua
module is `oorexx.so`, `oorexx.dll`; on Windows the `lua5.3` zip also has
`texlive/`, the module for TeX Live's LuaTeX). Every push to `main`
also makes them, as artifacts of the CI run, without a release.

## Getting started

You need a C++ compiler (on Windows, MSVC, used from Git Bash in a developer
command prompt's environment), an ooRexx 5.3.0 r13286 or later with its API
headers (the one installed in `/usr/local`, say, or a portable build) on the
`PATH` (`.LogicalWrapper` is in ooRexx since r13278; `WITH` and threads
registering command environments need the fixes of ooRexx bugs #2111 and
#2112, r13274 and r13275; threads, that of #2122, r13286), and a
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
oorexxlua/scripts/build.sh                   # librexxlua, oorexx.so and lua.cls, seconds
oorexxlua/examples/run-one.sh 01-tour.rex
oorexxlua/examples/run-one.sh 07-from-lua.lua  # needs a standalone lua: setup-lua.sh builds it
```

The build goes into `build/`, next to the clone, in the directory that
contains it (`$ORXLUA_WORK`, which you can override; `BUILD` overrides the
directory itself): `librexxlua.so` (`librexxlua.dylib` on macOS;
`rexxlua.dll` on Windows, with `lua54.dll`), the Lua module `oorexx.so`
(`oorexx.dll`), and `lua.cls`. To use it from your own programs, put the
build directory on `REXX_PATH` and on the library path (`LD_LIBRARY_PATH`,
`DYLD_LIBRARY_PATH` on macOS, `PATH` on Windows):
`LD_LIBRARY_PATH=build REXX_PATH=build rexx prog.rex`; from Lua, on
`LUA_CPATH` (`LUA_CPATH="build/?.so;;" lua prog.lua`; in LuaTeX, set
`package.cpath`).

## Tests

```bash
oorexxlua/scripts/setup-oorexxunit.sh        # ooRexx's test framework, once
oorexxlua/tests/run.sh
```

The tests are ooRexxUnit test groups (`tests/rexx/*.testGroup`) and, for the
Lua module, Lua scripts (`tests/lua/*.test.lua`, run by the standalone lua
that `setup-lua.sh` builds, or `LUA_EXE`), each run in its own process under
a timeout (a deadlock is a failure).
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
with the three Luas. `scripts/setup-luatex.sh` fetches TeX Live's LuaTeX,
to run the Lua module's tests in it: `LUA_EXE=$(oorexxlua/scripts/setup-luatex.sh)
LUA_VERSION=5.3 BUILD=build53 TESTS="Module.test.lua" oorexxlua/tests/run.sh`.

## Layout

- `src/` — the bridge.
  - `rexxlua.cpp` — the whole bridge: `librexxlua`, the ooRexx native
    package, with Lua linked in; and, built again without Lua, the Lua
    module `oorexx`.
  - `lua.cls` — the Rexx side: `.lua`, `.LuaState`, `.LuaObject` (its
    header gives the API).
- `examples/` — short programs, each with its exact output: `01-tour.rex`
  (one feature per section), `02-states.rex` (several Luas, closing,
  identity), `03-values.rex` (how values cross, `box`), `04-calls.rex`
  (the colon rule, several results, coroutines), `05-rexx-in-lua.rex`
  (Rexx objects used from Lua), `06-operators.rex`, `07-from-lua.lua` (the
  Lua module), `08-command-environments.rex` (Lua functions as `ADDRESS`
  environments); `run-one.sh`. `latex/`: a LuaLaTeX document with Rexx in it
  (`\rexx{...}`, and a Rexx program that writes into the page through
  `ADDRESS LUA`), not run by the tests.
- `tests/` — `run.sh`; `rexx/`, the ooRexxUnit test groups (examples,
  lifetime, threads, C modules, interrupts, and `Robustness`, a test for every fault a
  review found) with their base class `LuaTestCase.cls`; `lua/`, the tests
  of the Lua module, in Lua (`harness.lua`); `cmodule/` (a Lua C module built
  as LuaRocks builds them).
- `scripts/` — `build.sh` (the build), `platform.sh` (what differs between
  Linux, macOS and Windows), `setup-lua.sh` (builds a Lua from lua.org, and
  its standalone `lua`), `setup-luatex.sh` (fetches TeX Live's LuaTeX),
  `build-cmodule.sh` (the test C module), `package.sh` (the zip of
  binaries) and `setup-oorexxunit.sh` (fetches ooRexxUnit).
- `dist/` — what goes into the zip besides the binaries: its `README.md`,
  `check.rex` and `check.lua`.
- `.github/workflows/build.yml` — the CI, the packages and the releases.
- `docs/design.md` — how the bridge works and why, and what is not yet
  done.

## Credits

Like its sisters, this bridge follows BSF4ooRexx, the ooRexx–Java bridge by
Rony G. Flatscher.

Written by Josep Maria Blasco, with Claude (Anthropic) as coding assistant.

## License

Apache License 2.0 (`LICENSE`).
