/* cmodule.rex -- a C module not linked against liblua loads (docs/design.md,
   *C modules*); tests/run.sh builds it and sets LUA_CPATH                   */
hello = .lua~require("hello")
say hello~hi("Rexx")                               -- hello from C, Rexx

::requires "lua.cls"
