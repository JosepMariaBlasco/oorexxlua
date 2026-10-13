/* hello.c -- a Lua C module built as LuaRocks builds them: NOT linked against
   liblua (the lua_* symbols come from whoever loads it).  tests/run.sh builds
   it; tests/rexx/CModule.testGroup requires it, and Interrupt.testGroup uses
   its interrupt().                                                          */
#include <lua.h>
#include <lauxlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

static int hi(lua_State *L)
{
    lua_pushfstring(L, "hello from C, %s", luaL_checkstring(L, 1));
    return 1;
}

/* interrupt(): a Ctrl-C for this process, as if typed at its terminal.  On
   Unix a SIGINT, handled before raise returns.  On Windows a CTRL_C_EVENT,
   which goes to every process of a console: so first a console of its own;
   the handlers run on a thread of their own, so then a wait for them.      */
static int interrupt(lua_State *L)
{
#ifdef _WIN32
    FreeConsole();
    if (!AllocConsole()) return luaL_error(L, "interrupt: AllocConsole failed (%d)", (int)GetLastError());
    SetConsoleCtrlHandler(NULL, FALSE);         /* Ctrl-C not ignored */
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        return luaL_error(L, "interrupt: GenerateConsoleCtrlEvent failed (%d)", (int)GetLastError());
    Sleep(500);
#else
    raise(SIGINT);
#endif
    return 0;
}

int luaopen_hello(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, hi);
    lua_setfield(L, -2, "hi");
    lua_pushcfunction(L, interrupt);
    lua_setfield(L, -2, "interrupt");
    return 1;
}
