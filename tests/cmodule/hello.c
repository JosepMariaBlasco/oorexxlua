/* hello.c -- a Lua C module built as LuaRocks builds them: NOT linked against
   liblua (the lua_* symbols come from whoever loads it).  tests/run.sh builds
   it; tests/rexx/cmodule.rex requires it.                                   */
#include <lua.h>
#include <lauxlib.h>

static int hi(lua_State *L)
{
    lua_pushfstring(L, "hello from C, %s", luaL_checkstring(L, 1));
    return 1;
}

int luaopen_hello(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, hi);
    lua_setfield(L, -2, "hi");
    return 1;
}
