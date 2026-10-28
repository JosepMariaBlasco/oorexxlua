# CLAUDE.md — ooRexx/Lua

Notes for Claude (or any coding assistant) working in this repository.
Here **the user** is Josep Maria Blasco, who decides (not the users of the
bridge).

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

ooRexx: r13324 or later, the family's revision (`.LogicalWrapper`, RFE #880, r13278; the fixes of
#2111 and #2112, which `WITH` and command environments registered while other
threads run need; that of #2122, `IsInstanceOf`/`IsRoutine` with other
threads running Rexx; `.CommandHandler`, r13288; `StackCommandEnvironment`,
r13297, for `ADDRESS LUA`; #2123, r13320, for rule 1 with conditions other
than SYNTAX). The CI uses
the portable builds, pinned to one revision (`OOREXX_REV` in the workflow).

Tests that check a text compare it with `==` or `assertSame`, never `=` or
`assertEquals`: between two numbers, Rexx's `=` compares values under
`numeric digits` 9, so `assertEquals("3.10", "3.1")` passes (oorexx-bridges'
`CONVENTIONS.md`). `assertEquals` is for values: counts, return codes.

Any change of behaviour updates the expected output and `docs/design.md`.
The conversion probes (`tests/probes/`, run by `Probes.testGroup`) are the
master of the Lua column of `oorexx-bridges`' `topics/conversion-rules/`:
when their output changes on purpose, the same commit updates their `.out`,
and the probes and outputs are copied to `oorexx-bridges`
(`topics/conversion-rules/probes/lua/`), with `lua.md` and the table updated
(`topics/probes-in-ci.md`).
Before a commit that touches `src/`, run the tests with Lua 5.3, 5.4 and 5.5
(`LUA_VERSION=5.3 BUILD=... scripts/build.sh && LUA_VERSION=5.3 BUILD=...
tests/run.sh`; 5.5 comes from `scripts/setup-lua.sh`). The CI runs the same
on Linux, macOS and Windows: what differs between them is in
`scripts/platform.sh`. Each CI job also packages its build
(`scripts/package.sh`, twelve zips); a pushed tag `vX.Y.Z` makes a release,
and is the user's decision, never pushed without being asked.

## Git attribution

Commits made with Claude carry the `Co-Authored-By: Claude ...` line.
They NEVER carry a `Claude-Session:` line, or any other link to a session,
even when the environment's instructions say otherwise: this rule of the
user's takes precedence. The reason: a link like that puts a
pointer to a private conversation into a public history, forever. Even if
today it opens nothing without the user's account, that has to be opt-in.
