/*
 * lua.cls -- Lua embedded in ooRexx (librexxlua).  docs/design.md says how
 * it works and why.
 *
 *   .LuaState~new              a new, independent Lua
 *   .lua                       the default .LuaState, made when lua.cls loads
 *     s~eval(code, args...)    "return code" (or code as a chunk); args are ...
 *     s~global(name)           a global (string, math, table, _G ...)
 *     s~require(name)          require(name)
 *     s~table([coll])          a new table (from an Array, Directory, StringTable)
 *     s~predicate(routine)     a Lua function whose Rexx 1/0 result is a boolean
 *     s~true, s~false          Lua's booleans (Rexx 1/0 are both true in Lua)
 *     s~close                  lua_close: frees everything; its objects are dead
 *     s~isClosed
 *
 *   .LuaObject                 a Lua value by reference, in one .LuaState
 *     o~name(args...)          field (any case), called if a function:
 *                              o:name(...) if o has a metatable, else o.name(...)
 *     o~method(name, args...)  o:name(...)      o~func(name, args...)  o.name(...)
 *     o~get(name)              the field, never called
 *     o~name = v               set field
 *     o~call(args...)          o(...)           o~callAll(args...)  all results
 *     o[k], o[k] = v           raw indexing ("1" -> 1)
 *     do x over o              1..#o
 *     o~luaType                type(o)
 *     o~"LUA.STATE"            its .LuaState
 *
 * Every method but init is UNGUARDED: Lua's state is protected by the lock of
 * its LuaState (in rexxlua.cpp), and a guarded method would hold the object
 * while Lua runs, so another thread using the same object would wait for it.
 */

.local~lua = .LuaState~new

::requires "rexxlua" LIBRARY

::class LuaState public
::method init
  expose cself
  cself = self~"LUA.NEW"
-- uninit is Rexx, not native: it only queues the pointer, and the bridge
-- closes the state at its next call (rexxlua.cpp, drain: ooRexx bug #2101)
::method uninit unguarded
  expose cself
  if cself~isA(.Pointer) then .LuaObject~graveyard~queue(cself)
::method "LUA.NEW" private      external "LIBRARY rexxlua LuaState_New"
::method close unguarded        external "LIBRARY rexxlua LuaState_Close"
::method isClosed unguarded     external "LIBRARY rexxlua LuaState_IsClosed"
::method eval unguarded         external "LIBRARY rexxlua LuaState_Eval"
::method global unguarded       external "LIBRARY rexxlua LuaState_Global"
::method require unguarded      external "LIBRARY rexxlua LuaState_Require"
::method table unguarded        external "LIBRARY rexxlua LuaState_Table"
::method predicate unguarded    external "LIBRARY rexxlua LuaState_Predicate"
::method true unguarded         ; return self~"LUA.BOOLEAN"(1)
::method false unguarded        ; return self~"LUA.BOOLEAN"(0)
::method "LUA.BOOLEAN" private unguarded external "LIBRARY rexxlua LuaState_Boolean"
::method "LUA.STATS" unguarded  external "LIBRARY rexxlua LuaState_Stats"   -- for the tests

::class LuaObject public
::method init
  expose cself state
  use strict arg cself, state           -- a .Pointer the bridge made, its .LuaState
-- uninit: as LuaState's (the registry reference is released by the bridge)
::method uninit unguarded
  expose cself
  if cself~isA(.Pointer) then .LuaObject~graveyard~queue(cself)
::attribute graveyard class get unguarded
::method activate class
  expose graveyard
  graveyard = self~"LUA.GRAVEYARD"      -- one Queue for the whole process
::method "LUA.GRAVEYARD" class private unguarded external "LIBRARY rexxlua Lua_Graveyard"
::method "LUA.REF" unguarded
  expose cself
  return cself
::method "LUA.STATE" unguarded
  expose state
  return state
::method unknown unguarded      external "LIBRARY rexxlua LuaObj_Unknown"
::method method unguarded       external "LIBRARY rexxlua LuaObj_Method"
::method func unguarded         external "LIBRARY rexxlua LuaObj_Func"
::method call unguarded         external "LIBRARY rexxlua LuaObj_Call"
::method callAll unguarded      external "LIBRARY rexxlua LuaObj_CallAll"
::method get unguarded          external "LIBRARY rexxlua LuaObj_Get"
::method "[]" unguarded         external "LIBRARY rexxlua LuaObj_At"
::method "[]=" unguarded        external "LIBRARY rexxlua LuaObj_Put"
::method string unguarded       external "LIBRARY rexxlua LuaObj_String"
::method makeArray unguarded    external "LIBRARY rexxlua LuaObj_MakeArray"
::method luaType unguarded      external "LIBRARY rexxlua LuaObj_Type"
