/*
 * lua.cls -- the Rexx side of Lua 5.4 embedded in ooRexx (librexxlua).
 * A spike (10/10/2026), not yet a design: see docs/design.md.
 *
 *   .lua~eval(code, args...)   "return code" (or code as a chunk); args are ...
 *   .lua~global(name)          a global (string, math, table, _G ...)
 *   .lua~require(name)         require(name)
 *   .lua~table([coll])         a new table (from an Array, Directory, StringTable)
 *   .lua~predicate(routine)    a Lua function whose Rexx 1/0 result is a boolean
 *   .lua~true, .lua~false      Lua's booleans (Rexx 1/0 are both truthy in Lua)
 *
 *   .LuaObject                 a Lua value by reference
 *     o~name(args...)          field (any case), called if a function:
 *                              o:name(...) if o has a metatable, else o.name(...)
 *     o~method(name, args...)  o:name(...)      o~func(name, args...)  o.name(...)
 *     o~get(name)              the field, never called
 *     o~name = v               set field
 *     o~call(args...)          o(...)           o~callAll(args...)  all results
 *     o[k], o[k] = v           raw indexing ("1" -> 1)
 *     do x over o              1..#o
 *     o~luaType                type(o)
 */

.local~lua = .LuaBridge~new

::requires "rexxlua" LIBRARY

::class LuaBridge
::method eval     external "LIBRARY rexxlua Lua_Eval"
::method global   external "LIBRARY rexxlua Lua_Global"
::method require  external "LIBRARY rexxlua Lua_Require"
::method table    external "LIBRARY rexxlua Lua_Table"
::method predicate external "LIBRARY rexxlua Lua_Predicate"
::method true     ; return self~"LUA.BOOLEAN"(1)
::method false    ; return self~"LUA.BOOLEAN"(0)
::method "LUA.BOOLEAN" private external "LIBRARY rexxlua Lua_Boolean"

::class LuaObject public
::method init
  expose cself
  use strict arg cself
::method uninit
  self~"LUA.RELEASE"
::method "LUA.RELEASE" private external "LIBRARY rexxlua LuaObj_Release"
::method "LUA.REF"
  expose cself
  return cself
::method unknown   external "LIBRARY rexxlua LuaObj_Unknown"
::method method    external "LIBRARY rexxlua LuaObj_Method"
::method func      external "LIBRARY rexxlua LuaObj_Func"
::method call      external "LIBRARY rexxlua LuaObj_Call"
::method callAll   external "LIBRARY rexxlua LuaObj_CallAll"
::method get       external "LIBRARY rexxlua LuaObj_Get"
::method "[]"      external "LIBRARY rexxlua LuaObj_At"
::method "[]="     external "LIBRARY rexxlua LuaObj_Put"
::method string    external "LIBRARY rexxlua LuaObj_String"
::method makeArray external "LIBRARY rexxlua LuaObj_MakeArray"
::method luaType   external "LIBRARY rexxlua LuaObj_Type"
