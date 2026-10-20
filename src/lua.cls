/*
 * lua.cls -- Lua embedded in ooRexx (librexxlua).  docs/design.md says how
 * it works and why.
 *
 *   .LuaState~new              a new, independent Lua
 *   .lua                       the default .LuaState, made when lua.cls loads;
 *                              in Rexx run from Lua (require "oorexx"), the
 *                              state of that Lua
 *     s~eval(code, args...)    "return code" (or code as a chunk); args are ...
 *     s~global(name)           a global (string, math, table, _G ...)
 *     s~require(name)          require(name)
 *     s~table([coll])          a new table (from an Array, Directory, StringTable)
 *     s~predicate(routine)     a Lua function whose Rexx 1/0 result is a boolean
 *     s~box(type, value)       value as a Lua boolean, integer, float or string,
 *                              or (type "reference") a Rexx object by reference
 *     s~true, s~false          .LogicalWrapper~true, ~false (aliases): Lua's
 *                              booleans (Rexx 1/0 are both true in Lua)
 *   .LogicalWrapper            ooRexx's logicalwrapper.cls (r13278): its
 *                              two instances are Lua's true and false, both ways
 *     s~close                  lua_close: frees everything; its objects are dead
 *     s~isClosed
 *
 *   ADDRESS LUA                commands are Lua code, run in .lua: RC 0 and
 *                              the results in RESULT, or ERROR / FAILURE with
 *                              Lua's message as condition('A')
 *     s~addCommandEnvironment(name, handler)
 *                              ADDRESS name runs handler(command, ctx), a Lua
 *                              function (a .LuaObject), in its state: RC its
 *                              value; again: replaced
 *     s~removeCommandEnvironment(name)
 *                              ADDRESS name: FAILURE RC 30 from now on
 *
 *   .LuaObject                 a Lua value by reference, in one .LuaState
 *     o~name(args...)          field (any case), called if a function, as
 *                              o:name(...) or o.name(...): o:name if it is a
 *                              Lua function whose first parameter is self, or
 *                              a C function found through the metatable
 *     o~all(name, args...)     the same, every result as an Array
 *     o~method(name, args...)  o:name(...)      o~func(name, args...)  o.name(...)
 *     o~get(name)              the field, never called
 *     o~name = v               set field
 *     o~call(args...)          o(...)           o~callAll(args...)  all results
 *     o[k], o[k] = v           indexing (k converted as any value: "1" -> 1)
 *     do x over o              1..#o
 *     o~luaType                type(o)
 *     o + x, -o, o < x ...     Lua's operators (metamethods too); = is Lua's ==,
 *                              == is rawequal (and hashCode agrees with it)
 *     o~"LUA.STATE"            its .LuaState
 *
 * Every method but init is UNGUARDED: Lua's state is protected by the lock of
 * its LuaState (in rexxlua.cpp), and a guarded method would hold the object
 * while Lua runs, so another thread using the same object would wait for it.
 */

.LuaState~"LUA.LOGICALWRAPPER"(.LogicalWrapper)   -- for the bridge (rexxlua.cpp)
.local~lua = .LuaState~"LUA.DEFAULT"
.LuaState~"LUA.ADDRESS"                 -- ADDRESS LUA (in rexxlua.cpp)

::requires "rexxlua" LIBRARY
::requires "logicalwrapper.cls"         -- .LogicalWrapper: ooRexx's (r13278), the family's boolean class

::class LuaState public
::method init
  expose cself
  cself = self~"LUA.NEW"
-- .lua: the state of the Lua that runs this instance of ooRexx (the Lua
-- module, docs/design.md, *Lua → ooRexx*: LUA.ADOPTNEXT makes LUA.NEW adopt
-- it), or a new one
::method "LUA.DEFAULT" class unguarded
  self~"LUA.ADOPTNEXT"
  return self~new
::method "LUA.ADOPTNEXT" class unguarded external "LIBRARY rexxlua Lua_AdoptNext"
::method "LUA.ADDRESS" class unguarded  external "LIBRARY rexxlua Lua_Address"
::method "LUA.LOGICALWRAPPER" class unguarded external "LIBRARY rexxlua Lua_LogicalWrapper"
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
::method box unguarded          external "LIBRARY rexxlua LuaState_Box"
::method addCommandEnvironment unguarded    external "LIBRARY rexxlua LuaState_AddCommandEnvironment"
::method removeCommandEnvironment unguarded external "LIBRARY rexxlua LuaState_RemoveCommandEnvironment"
::method true unguarded         ; return .LogicalWrapper~true     -- (aliases: Lua's true and false)
::method false unguarded        ; return .LogicalWrapper~false
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
::method all unguarded          external "LIBRARY rexxlua LuaObj_All"
::method call unguarded         external "LIBRARY rexxlua LuaObj_Call"
::method callAll unguarded      external "LIBRARY rexxlua LuaObj_CallAll"
::method get unguarded          external "LIBRARY rexxlua LuaObj_Get"
::method "[]" unguarded         external "LIBRARY rexxlua LuaObj_At"
::method "[]=" unguarded        external "LIBRARY rexxlua LuaObj_Put"
::method string unguarded       external "LIBRARY rexxlua LuaObj_String"
::method makeArray unguarded    external "LIBRARY rexxlua LuaObj_MakeArray"
::method supplier unguarded     external "LIBRARY rexxlua LuaObj_Supplier"
::method makeStringTable unguarded external "LIBRARY rexxlua LuaObj_MakeStringTable"
::method luaType unguarded      external "LIBRARY rexxlua LuaObj_Type"
-- Rexx operators: Lua's (a + b, -a, a < b...); == is identity (rawequal)
::method "+" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "-" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "*" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "/" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "%" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "//" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method "**" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method "<" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method ">" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "<=" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method ">=" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method "=" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "\=" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method "<>" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method "><" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method "==" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method "\==" unguarded    external "LIBRARY rexxlua LuaObj_Operator"
::method "&" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "|" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method "&&" unguarded     external "LIBRARY rexxlua LuaObj_Operator"
::method "\" unguarded      external "LIBRARY rexxlua LuaObj_Operator"
::method hashCode unguarded     external "LIBRARY rexxlua LuaObj_HashCode"
