/* check.rex: is the ooRexx/Lua bridge installed?
 *   rexx check.rex
 * Needs this folder on REXX_PATH (lua.cls) and on the library path (the
 * package, librexxlua / rexxlua.dll, and on Windows luaXY.dll).
 * ORXLUA_CHECK_MODULE=name also requires a Lua C module of that name (found
 * through LUA_CPATH) and calls its hi("Rexx"): package.sh checks this way
 * that C modules load.
 */
say "ooRexx/Lua: ooRexx" .rexxInfo~version "r".rexxInfo~revision"," .lua~global("_VERSION")
ok = 1
if .lua~eval("2^10") \== "1024.0" then ok = 0                      -- a float, exactly
t = .lua~table(.array~of(3, 1, 2))
.lua~global("table")~sort(t, .lua~predicate(.routine~new("gt", "return arg(1) > arg(2)")))
if .lua~global("table")~concat(t, ",") \== "3,2,1" then ok = 0     -- a Rexx callback
module = value("ORXLUA_CHECK_MODULE", , "ENVIRONMENT")
if module \== "" then do
  hi = .lua~require(module)~hi("Rexx")
  say "ooRexx/Lua: the C module" module "says:" hi
  if hi~pos("Rexx") = 0 then ok = 0
end
if \ok then do
  say "ooRexx/Lua: FAILED"
  exit 1
end
say "ooRexx/Lua: OK"

::requires "lua.cls"
