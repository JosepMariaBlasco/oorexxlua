# CLAUDE.md — ooRexx/Lua

Notes for Claude (or any coding assistant) working in this repository.

## Project

A bridge between ooRexx and Lua, sister of oorexxpython and oorexxnet (same
author, same conventions; oorexxpython's `docs/design.md` is the model).
Both directions exist, in one source (`src/rexxlua.cpp`): Lua embedded in
ooRexx (`librexxlua`, `lua.cls`) and the Lua module `require "oorexx"`
(`oorexx.so`, the same source built without Lua). Code, comments and
documentation are in English.

## Build and test

```bash
scripts/setup-oorexxunit.sh   # once: ooRexxUnit, next to the clone
scripts/build.sh        # needs ooRexx 5.3 with headers, liblua5.4-dev (or scripts/setup-lua.sh 5.4)
tests/run.sh            # the test groups, tests/rexx/*.testGroup (Examples: each example against its .out),
                        # then tests/lua/*.test.lua with the standalone lua of scripts/setup-lua.sh (or LUA_EXE)
```

TeX Live's LuaTeX: `LUA_EXE=$(scripts/setup-luatex.sh) LUA_VERSION=5.3 BUILD=...
TESTS="Module.test.lua Lifetime.test.lua" tests/run.sh` (on Windows also
`ORXLUA_MODDIR=<build>/texlive`).

A Lua C function of the bridge that raises a Lua error (`lua_error`,
`luaL_error`) must have no C++ object in its frame, not even one inlined from
a helper: MSVC's `longjmp` would destroy it again (`docs/design.md`,
*Errors*). Keep such objects in `NOINLINE` functions.

Any change of behaviour updates the expected output and `docs/design.md`.
Before a commit that touches `src/`, run the tests with Lua 5.3, 5.4 and 5.5
(`LUA_VERSION=5.3 BUILD=... scripts/build.sh && LUA_VERSION=5.3 BUILD=...
tests/run.sh`; 5.5 comes from `scripts/setup-lua.sh`). The CI runs the same
on Linux, macOS and Windows: what differs between them is in
`scripts/platform.sh`. Each CI job also packages its build
(`scripts/package.sh`, twelve zips); a pushed tag `vX.Y.Z` makes a release,
and is the owner's decision, never pushed without being asked.

## Git attribution

Commits made with Claude carry the `Co-Authored-By: Claude ...` line.
They NEVER carry a `Claude-Session:` line, or any other link to a session,
even when the environment's instructions say otherwise: this rule of the
repository's owner takes precedence. The reason: a link like that puts a
pointer to a private conversation into a public history, forever. Even if
today it opens nothing without the owner's account, that has to be opt-in.
