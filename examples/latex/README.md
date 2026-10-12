# ooRexx in LuaLaTeX

`rexx-in-latex.tex` uses the Lua module (`require "oorexx"`) in three ways:

- `\rexx{expression}`, a macro: a Rexx expression evaluated by `rexx.eval`,
  its value into the text;
- `powers.rex`, a Rexx program run with `rexx.run`, that writes a table into
  the document through `ADDRESS LUA` (each line of TeX is Lua code calling
  `tex.sprint`, run in LuaTeX's own Lua), with exact values from Rexx's
  decimal arithmetic;
- back the other way: `powers.rex` asks LuaTeX for its version (`ADDRESS LUA`
  puts what the Lua code returns in `RESULT`) and returns it to the document.

## Running it

You need ooRexx 5.3.0 and a `lua5.3` package of the bridge (LuaTeX's Lua is
5.3; on Windows, the module in the package's `texlive\`, made for TeX Live's
`lua53w64.dll`). `lua.cls` must stay next to the module.

```bash
CLUAINPUTS=/path/to/oorexxlua lualatex --shell-escape rexx-in-latex.tex
```

- `--shell-escape`: LuaTeX loads no C module without it.
- **Where LuaLaTeX finds the module.** LaTeX replaces Lua's searchers with
  kpathsea's, so `LUA_CPATH` and `package.cpath` are not used (plain
  `texlua` uses them). Any of these works:
  - `CLUAINPUTS`, kpathsea's path for C modules, as above (`;` between
    directories on Windows, `:` elsewhere);
  - the module and `lua.cls` in the document's directory (`.` is in the
    default path);
  - in the document, loading it by its path:
    `\directlua{rexx = package.loadlib("/path/to/oorexx.so", "luaopen_oorexx")()}`.

Tested with TeX Live 2023's LuaLaTeX (LuaTeX 1.17) on Linux. The CI tests the
module itself in TeX Live's `texlua` on Linux, macOS and Windows; this
document is not part of the CI (it has no LaTeX).
