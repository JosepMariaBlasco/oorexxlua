# ooRexx/Lua — binaries

Lua embedded in ooRexx: ooRexx programs run Lua code, use Lua's libraries and
modules as objects, and pass Rexx objects and routines to Lua.
Source, documentation and design: https://github.com/JosepMariaBlasco/oorexxlua

**Status: prototype.** Parts of the API may still change.

## What you need

- **ooRexx 5.3.0** (beta, from
  [SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/5.3.0beta/)),
  installed so that `rexx` runs.
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
