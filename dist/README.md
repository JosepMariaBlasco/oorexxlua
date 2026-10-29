# ooRexx/Lua — binaries

Lua embedded in ooRexx: ooRexx programs run Lua code, use Lua's libraries and
modules as objects, and pass Rexx objects and routines to Lua. And the other
way round: Lua programs (LuaTeX's too) run Rexx code with
`require "oorexx"`.
Source, documentation and design: https://github.com/JosepMariaBlasco/oorexxlua

**Status: prototype.** Parts of the API may still change.

## What you need

- **ooRexx 5.3.0, r13329 or later** (beta, from
  [SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/5.3.0beta/)),
  installed so that `rexx` runs (`rexx -v` gives the revision).
- **No Lua**: this package carries its own (the version in the zip's name;
  `LICENSE-Lua.txt` says which release). Pick the Lua version your Lua code
  and your Lua C modules are made for; 5.4 if unsure.
- The Linux binaries are built on Ubuntu 24.04 (x86_64); an older
  distribution may need to build from source.

## Install

Unzip anywhere. The folder `oorexxlua` holds everything; tell ooRexx where
it is. With the folder at `/path/to/oorexxlua`:

**Linux**

```bash
export LD_LIBRARY_PATH=/path/to/oorexxlua:$LD_LIBRARY_PATH
export REXX_PATH=/path/to/oorexxlua
```

**macOS**: the same, with `DYLD_LIBRARY_PATH` instead of `LD_LIBRARY_PATH`.
macOS drops `DYLD_*` variables when it starts a system program such as
`/bin/sh`, so a command run through a shell (ooRexx's `ADDRESS SYSTEM`,
`make`...) must set it again itself.

**Windows** (Command Prompt; ooRexx's `bin` directory must be on the `PATH`
too):

```bat
set PATH=C:\path\to\oorexxlua;%PATH%
set REXX_PATH=C:\path\to\oorexxlua
```

## Check

```
rexx check.rex
```

It ends with `ooRexx/Lua: OK`.

## Use

`::requires "lua.cls"`, and `.lua`:

```rexx
say .lua~eval("2^10")                       -- 1024.0
string = .lua~global("string")
say string~rep("ab", 3, "-")                -- ab-ab-ab

::requires "lua.cls"
```

The `examples` folder has short examples of every feature, each with its
exact output (`NAME.out`). The API is in the header of `lua.cls`.

## Rexx from Lua: the Lua module

`oorexx.so` (`oorexx.dll` on Windows) is a Lua C module for the Lua version
in the zip's name (5.3 for LuaTeX): `require "oorexx"`.

```lua
local rexx = require "oorexx"
print(rexx.eval("6 * 7"))                   -- 42
local f = rexx.routine("use arg a, b; return a * b")
print(f(6, 7))                              -- 42
```

Lua finds it through `LUA_CPATH` (`lua.cls` must stay next to it):
`export LUA_CPATH="/path/to/oorexxlua/?.so;;"` (on Windows,
`set LUA_CPATH=C:\path\to\oorexxlua\?.dll;;`). It needs ooRexx's
libraries (`librexx`, `librexxapi`; `rexx.dll`, `rexxapi.dll`) where the
system finds them: on Windows, ooRexx's `bin` on the `PATH`; on Linux and
macOS, an ooRexx installed in `/usr/local` is found, and a portable one
needs its `lib` directory on `LD_LIBRARY_PATH` (`DYLD_LIBRARY_PATH`).
`lua check.lua` checks it (it ends with `ooRexx/Lua module: OK`); the API is
in `examples/07-from-lua.lua`.

**LuaTeX** (LuaLaTeX...) is Lua 5.3: use a `lua5.3` zip, and run with
`--shell-escape` (LuaTeX loads no C module without it). On Windows, TeX
Live's LuaTeX needs the module in `texlive\` (its Lua is `lua53w64.dll`).
LaTeX replaces Lua's searchers with kpathsea's, so LuaLaTeX ignores
`LUA_CPATH` and `package.cpath`: give the module's directory in
`CLUAINPUTS` (`CLUAINPUTS=/path/to/oorexxlua lualatex --shell-escape doc.tex`),
or put the module and `lua.cls` next to the document, or load it by its path
in the document:

```latex
\directlua{rexx = package.loadlib("/path/to/oorexxlua/oorexx.so", "luaopen_oorexx")()}
\newcommand\rexx[1]{\directlua{tex.sprint(rexx.eval("\luaescapestring{\detokenize{#1}}"))}}
$2^{100}$ is \rexx{2**100}; a price: \rexx{format(3.1, , 2)} EUR.
```

A whole example, with a Rexx program that writes into the document through
`ADDRESS LUA`, is in `examples/latex/`.

`texlua check.lua` checks it (with `LUA_CPATH` set as above).

## Lua modules

Modules written in Lua are found through `LUA_PATH`, as in any Lua. C
modules (LuaRocks's, or a distribution's) are found through `LUA_CPATH`, and
must be built for the same Lua version (5.4 modules for a `lua5.4` package):

- on Linux and macOS, modules built the usual way (not linked against
  `liblua`) use the Lua inside this package. Its default paths are
  lua.org's (`/usr/local/lib/lua/5.4/`...); a distribution's modules are
  elsewhere, so tell it, for example on Debian and Ubuntu (`lua-lpeg` and
  friends):
  `export LUA_CPATH="/usr/lib/x86_64-linux-gnu/lua/5.4/?.so;;"`;
- on Windows, modules are linked against the Lua DLL by name: they must use
  the `luaXY.dll` in this folder (`lua54.dll` for 5.4), which must be found
  first on the `PATH`.

## Licence

Apache License 2.0 (`LICENSE`). Lua: MIT licence (`LICENSE-Lua.txt`).
