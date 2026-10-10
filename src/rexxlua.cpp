/*
 * rexxlua.cpp -- librexxlua.so: Lua 5.4 embedded in ooRexx, as a native
 * package.  Started as a spike (10/10/2026), not yet a design: one lua_State
 * per process, single-threaded, no locks.  docs/design.md says what it shows
 * and which questions it leaves open.
 *
 * Rexx side: lua.cls (.lua, .LuaObject)
 *
 * Values, Lua -> Rexx:  nil -> .nil; boolean -> 1/0; integer -> "%lld";
 *   float -> Lua's own tostring; string -> string (bytes, unchanged);
 *   a wrapped Rexx object -> that object; anything else -> .LuaObject proxy.
 * Values, Rexx -> Lua:  .nil -> nil; .LuaObject -> its value; Array -> a new
 *   sequence table (copy); string -> string (Lua coerces strings to numbers
 *   in arithmetic); any other object -> userdata "RexxObject" that Lua can
 *   call (Routines) or send messages to (obj:name(...)).
 *   Keys of [] and []= follow the canonical-integer rule instead: "1" -> 1.
 */

#include <oorexxapi.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <string>
#include <cstring>
#include <cctype>
#include <cstdint>

static lua_State *L = NULL;
static RexxThreadContext *cur = NULL;   // thread running the current bridge call

static const char *REXXOBJ = "RexxObject";

/* ------------------------------------------------------------------------ */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------ */

static bool isCanonicalInteger(const char *s, size_t n)
{
    if (n == 0 || n > 18) return false;
    size_t i = 0;
    if (s[0] == '-') { if (n == 1) return false; i = 1; }
    if (s[i] == '0' && n > i + 1) return false;
    for (; i < n; i++) if (!isdigit((unsigned char)s[i])) return false;
    if (s[0] == '-' && n == 2 && s[1] == '0') return false;
    return true;
}

static RexxObjectPtr toRexx(RexxThreadContext *c, int idx);
static void pushValue(RexxThreadContext *c, RexxObjectPtr v, bool asKey = false);

/* Run the function and nargs arguments on top of the stack, protected.
 * Leaves all results on the stack and returns their count, or -1 after
 * raising a Rexx SYNTAX 98.900 with Lua's message and traceback.          */
static int msgh(lua_State *L)
{
    const char *m = lua_tostring(L, 1);
    if (m == NULL) m = luaL_tolstring(L, 1, NULL);
    luaL_traceback(L, L, m, 1);
    return 1;
}

static int pcallAll(RexxThreadContext *c, int nargs)
{
    int base = lua_gettop(L) - nargs;       // index of the function
    lua_pushcfunction(L, msgh);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, LUA_MULTRET, base);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        std::string m = "Lua error: ";
        m += lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error object)";
        lua_pop(L, 1);
        c->RaiseException1(Rexx_Error_Execution_user_defined, c->String(m.c_str()));
        return -1;
    }
    return lua_gettop(L) - base + 1;
}

/* First result as a Rexx object (.nil if none); pops all results. */
static RexxObjectPtr firstResult(RexxThreadContext *c, int n)
{
    if (n < 0) return NULLOBJECT;
    RexxObjectPtr r = n == 0 ? c->Nil() : toRexx(c, -n);
    lua_pop(L, n);
    return r;
}

/* All results as a Rexx Array; pops them. */
static RexxObjectPtr allResults(RexxThreadContext *c, int n)
{
    if (n < 0) return NULLOBJECT;
    RexxArrayObject a = c->NewArray(n);
    for (int i = 1; i <= n; i++) c->ArrayPut(a, toRexx(c, -n + i - 1), i);
    lua_pop(L, n);
    return a;
}

/* ------------------------------------------------------------------------ */
/* Rexx objects inside Lua: userdata "RexxObject"                              */
/* ------------------------------------------------------------------------ */

struct RexxBox { RexxObjectPtr obj; };

static int rexxCallBack(lua_State *L, RexxObjectPtr target, const char *message, int first)
{
    RexxThreadContext *c = cur;
    int n = lua_gettop(L);
    RexxArrayObject args = c->NewArray(n >= first ? n - first + 1 : 0);
    for (int i = first; i <= n; i++) {
        RexxObjectPtr v = toRexx(c, i);
        if (v != c->Nil()) c->ArrayPut(args, v, i - first + 1);
    }
    RexxObjectPtr r;
    if (message == NULL && c->IsRoutine(target))
        r = c->CallRoutine((RexxRoutineObject)target, args);
    else
        r = c->SendMessage(target, message ? message : "CALL", args);
    if (c->CheckCondition()) {
        RexxCondition cd;
        c->DecodeConditionInfo(c->GetConditionInfo(), &cd);
        c->ClearCondition();
        std::string m = "Rexx error " + std::to_string(cd.code / 1000) + "." + std::to_string(cd.code % 1000);
        if (cd.message) { m += ": "; m += c->CString(cd.message); }
        else if (cd.errortext) { m += ": "; m += c->CString(cd.errortext); }
        lua_pushstring(L, m.c_str());
        return lua_error(L);
    }
    if (r == NULLOBJECT) return 0;
    pushValue(c, r);
    return 1;
}

static int box_call(lua_State *L)       // rexxobj(...)
{
    RexxBox *b = (RexxBox *)luaL_checkudata(L, 1, REXXOBJ);
    return rexxCallBack(L, b->obj, NULL, 2);
}

static int box_method(lua_State *L)     // closure from __index: rexxobj:name(...)
{
    RexxBox *b = (RexxBox *)lua_touserdata(L, lua_upvalueindex(1));
    const char *name = lua_tostring(L, lua_upvalueindex(2));
    // obj:name(...) passes obj first: skip it; obj.name(...) does not
    int first = (lua_touserdata(L, 1) == b) ? 2 : 1;
    return rexxCallBack(L, b->obj, name, first);
}

static int box_index(lua_State *L)
{
    luaL_checkudata(L, 1, REXXOBJ);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, box_method, 2);
    return 1;
}

// A Rexx Routine as a real Lua function (table.sort & co. check the type).
// upvalue 1: the box; upvalue 2: true for a predicate (Rexx 1/0 -> boolean)
static int routine_closure(lua_State *L)
{
    RexxBox *b = (RexxBox *)lua_touserdata(L, lua_upvalueindex(1));
    bool predicate = lua_toboolean(L, lua_upvalueindex(2));
    int n = rexxCallBack(L, b->obj, NULL, 1);
    if (predicate && n == 1) {
        size_t len; const char *s = lua_tolstring(L, -1, &len);
        bool t = s && len == 1 && s[0] == '1';
        lua_pop(L, 1);
        lua_pushboolean(L, t);
    }
    return n;
}

static void pushBox(RexxThreadContext *c, RexxObjectPtr v)
{
    RexxBox *b = (RexxBox *)lua_newuserdatauv(L, sizeof(RexxBox), 0);
    b->obj = c->RequestGlobalReference(v);
    luaL_setmetatable(L, REXXOBJ);
}

static void pushRoutine(RexxThreadContext *c, RexxObjectPtr v, bool predicate)
{
    pushBox(c, v);
    lua_pushboolean(L, predicate);
    lua_pushcclosure(L, routine_closure, 2);
}

static int box_tostring(lua_State *L)
{
    RexxBox *b = (RexxBox *)luaL_checkudata(L, 1, REXXOBJ);
    RexxStringObject s = cur->ObjectToString(b->obj);
    lua_pushlstring(L, cur->StringData(s), cur->StringLength(s));
    return 1;
}

static int box_gc(lua_State *L)
{
    RexxBox *b = (RexxBox *)luaL_checkudata(L, 1, REXXOBJ);
    if (cur && b->obj) cur->ReleaseGlobalReference(b->obj);
    b->obj = NULL;
    return 0;
}

static void ensureLua()
{
    if (L) return;
    L = luaL_newstate();
    luaL_openlibs(L);
    luaL_newmetatable(L, REXXOBJ);
    static const luaL_Reg mt[] = {
        {"__call", box_call}, {"__index", box_index},
        {"__tostring", box_tostring}, {"__gc", box_gc}, {NULL, NULL}};
    luaL_setfuncs(L, mt, 0);
    lua_pop(L, 1);
}

/* ------------------------------------------------------------------------ */
/* Conversions                                                                */
/* ------------------------------------------------------------------------ */

static RexxObjectPtr newProxy(RexxThreadContext *c, int idx)
{
    lua_pushvalue(L, idx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    RexxClassObject cls = c->FindClass("LUAOBJECT");
    return c->SendMessage1(cls, "NEW", c->NewPointer((POINTER)(intptr_t)ref));
}

static RexxObjectPtr toRexx(RexxThreadContext *c, int idx)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:     return c->Nil();
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? c->True() : c->False();
    case LUA_TNUMBER: {                                 // Lua's own format, on a copy
        lua_pushvalue(L, idx);
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        RexxObjectPtr r = c->NewString(s, len);
        lua_pop(L, 1);
        return r;
    }
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        return c->NewString(s, len);
    }
    case LUA_TUSERDATA: {
        RexxBox *b = (RexxBox *)luaL_testudata(L, idx, REXXOBJ);
        if (b) return b->obj;
        return newProxy(c, idx);
    }
    default:
        return newProxy(c, idx);
    }
}

static void pushValue(RexxThreadContext *c, RexxObjectPtr v, bool asKey)
{
    if (v == NULLOBJECT || v == c->Nil()) { lua_pushnil(L); return; }
    if (c->IsOfType(v, "LUAOBJECT")) {
        RexxObjectPtr p = c->SendMessage0(v, "LUA.REF");
        int ref = (int)(intptr_t)c->PointerValue((RexxPointerObject)p);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        return;
    }
    if (c->IsArray(v)) {
        RexxArrayObject a = (RexxArrayObject)v;
        size_t n = c->ArrayItems(a);
        size_t size = c->ArraySize(a);
        lua_createtable(L, (int)n, 0);
        for (size_t i = 1; i <= size; i++) {
            RexxObjectPtr item = c->ArrayAt(a, i);
            if (item == NULLOBJECT) continue;
            pushValue(c, item);
            lua_rawseti(L, -2, (lua_Integer)i);
        }
        return;
    }
    if (c->IsString(v) || c->IsOfType(v, "STRING")) {   // Integers from literals too
        RexxStringObject s = c->ObjectToString(v);
        const char *d = c->StringData(s);
        size_t n = c->StringLength(s);
        if (asKey && isCanonicalInteger(d, n)) lua_pushinteger(L, (lua_Integer)strtoll(d, NULL, 10));
        else lua_pushlstring(L, d, n);
        return;
    }
    // A Routine: a real Lua function. Any other object: a userdata Lua can
    // send messages to (obj:name(...)).
    if (c->IsRoutine(v)) { pushRoutine(c, v, false); return; }
    pushBox(c, v);
}

static void pushArgs(RexxThreadContext *c, RexxArrayObject args, int &nargs)
{
    size_t size = args ? c->ArraySize(args) : 0;
    for (size_t i = 1; i <= size; i++) pushValue(c, c->ArrayAt(args, i));
    nargs = (int)size;
}

static void pushSelf(RexxThreadContext *c, POINTER self)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, (int)(intptr_t)self);
}

/* Case-insensitive name resolution on the value at the top of the stack.
 * Rexx uppercases names: try lowercase, then as written, then (tables only)
 * a scan of the keys. Pushes the value found (possibly nil).             */
static void resolveName(const char *name)
{
    std::string lower(name);
    for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
    int obj = lua_gettop(L);
    lua_getfield(L, obj, lower.c_str());
    if (!lua_isnil(L, -1)) return;
    lua_pop(L, 1);
    lua_getfield(L, obj, name);
    if (!lua_isnil(L, -1) || lua_type(L, obj) != LUA_TTABLE) return;
    lua_pop(L, 1);
    lua_pushnil(L);
    while (lua_next(L, obj)) {
        if (lua_type(L, -2) == LUA_TSTRING && strcasecmp(lua_tostring(L, -2), name) == 0) {
            lua_remove(L, -2);      // keep value
            return;
        }
        lua_pop(L, 1);
    }
    lua_pushnil(L);
}

/* ------------------------------------------------------------------------ */
/* .lua                                                                       */
/* ------------------------------------------------------------------------ */

// .lua~eval(code, args...): "return code" if it compiles, else code as a chunk
RexxMethod2(RexxObjectPtr, Lua_Eval, CSTRING, code, ARGLIST, args)
{
    cur = context->threadContext; ensureLua();
    std::string ret = std::string("return ") + code;
    if (luaL_loadbuffer(L, ret.c_str(), ret.size(), "=eval") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, code, strlen(code), "=eval") != LUA_OK) {
            std::string m = std::string("Lua error: ") + lua_tostring(L, -1);
            lua_pop(L, 1);
            context->RaiseException1(Rexx_Error_Execution_user_defined, context->String(m.c_str()));
            return NULLOBJECT;
        }
    }
    int nargs = 0;
    size_t size = context->ArraySize(args);
    for (size_t i = 2; i <= size; i++) pushValue(context->threadContext, context->ArrayAt(args, i));
    nargs = size > 1 ? (int)size - 1 : 0;
    return firstResult(context->threadContext, pcallAll(context->threadContext, nargs));
}

// .lua~global(name), .lua~require(name), .lua~true, .lua~false
RexxMethod1(RexxObjectPtr, Lua_Global, CSTRING, name)
{
    cur = context->threadContext; ensureLua();
    lua_getglobal(L, name);
    RexxObjectPtr r = toRexx(context->threadContext, -1);
    lua_pop(L, 1);
    return r;
}

RexxMethod1(RexxObjectPtr, Lua_Require, CSTRING, name)
{
    cur = context->threadContext; ensureLua();
    lua_getglobal(L, "require");
    lua_pushstring(L, name);
    return firstResult(context->threadContext, pcallAll(context->threadContext, 1));
}

RexxMethod1(RexxObjectPtr, Lua_Boolean, logical_t, b)
{
    cur = context->threadContext; ensureLua();
    lua_pushboolean(L, b ? 1 : 0);
    RexxObjectPtr r = newProxy(context->threadContext, -1);
    lua_pop(L, 1);
    return r;
}

// .lua~predicate(routine): a Lua function whose Rexx 1/0 result is a boolean
RexxMethod1(RexxObjectPtr, Lua_Predicate, RexxObjectPtr, routine)
{
    cur = context->threadContext; ensureLua();
    pushRoutine(context->threadContext, routine, true);
    RexxObjectPtr r = newProxy(context->threadContext, -1);
    lua_pop(L, 1);
    return r;
}

// .lua~table(array | directory | stringtable): a new Lua table (copy)
RexxMethod1(RexxObjectPtr, Lua_Table, OPTIONAL_RexxObjectPtr, src)
{
    cur = context->threadContext; ensureLua();
    RexxThreadContext *c = context->threadContext;
    if (src == NULLOBJECT) lua_newtable(L);
    else if (c->IsArray(src)) pushValue(c, src);
    else if (c->IsDirectory(src) || c->IsStringTable(src)) {
        lua_newtable(L);
        RexxObjectPtr sup = c->SendMessage0(src, "SUPPLIER");
        while (c->SupplierAvailable((RexxSupplierObject)sup)) {
            pushValue(c, c->SupplierIndex((RexxSupplierObject)sup), true);
            pushValue(c, c->SupplierItem((RexxSupplierObject)sup));
            lua_settable(L, -3);
            c->SupplierNext((RexxSupplierObject)sup);
        }
    }
    else lua_newtable(L);
    RexxObjectPtr r = newProxy(c, -1);
    lua_pop(L, 1);
    return r;
}

/* ------------------------------------------------------------------------ */
/* .LuaObject                                                                  */
/* ------------------------------------------------------------------------ */

// o~name(args...): the field (any case); if a function, called.
//   SPIKE HEURISTIC for the colon problem: when o has a metatable the call is
//   o:name(...) (self passed), otherwise o.name(...). ~method and ~func force.
// o~name=v: sets the field.
static RexxObjectPtr callField(RexxMethodContext *context, POINTER self, CSTRING message,
                               RexxArrayObject args, int colon /* -1 auto, 0, 1 */, bool all)
{
    RexxThreadContext *c = context->threadContext;
    cur = c; ensureLua();
    std::string name = message;
    size_t size = args ? c->ArraySize(args) : 0;
    pushSelf(c, self);
    int obj = lua_gettop(L);
    if (name.size() > 1 && name.back() == '=' && colon == -1) {      // assignment
        name.pop_back();
        std::string lower(name);
        for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
        lua_getfield(L, obj, name.c_str());                         // keep an existing case
        const char *key = lua_isnil(L, -1) ? lower.c_str() : name.c_str();
        lua_pop(L, 1);
        pushValue(c, size ? c->ArrayAt(args, 1) : NULLOBJECT);
        lua_setfield(L, obj, key);
        lua_pop(L, 1);
        return NULLOBJECT;
    }
    resolveName(name.c_str());
    bool callable = lua_type(L, -1) == LUA_TFUNCTION;
    if (!callable && luaL_getmetafield(L, -1, "__call") != LUA_TNIL) { lua_pop(L, 1); callable = true; }
    if (!callable) {
        if (size > 0) {
            lua_pop(L, 2);
            std::string m = "Lua error: field '" + name + "' is not callable";
            c->RaiseException1(Rexx_Error_Execution_user_defined, c->String(m.c_str()));
            return NULLOBJECT;
        }
        RexxObjectPtr r = toRexx(c, -1);
        lua_pop(L, 2);
        return r;
    }
    if (colon == -1) {
        colon = lua_getmetatable(L, obj) ? 1 : 0;
        if (colon) lua_pop(L, 1);
    }
    int nargs = 0;
    if (colon) { lua_pushvalue(L, obj); nargs = 1; }
    for (size_t i = 1; i <= size; i++) pushValue(c, c->ArrayAt(args, i));
    nargs += (int)size;
    int n = pcallAll(c, nargs);
    RexxObjectPtr r = all ? allResults(c, n) : firstResult(c, n);
    lua_pop(L, 1);          // obj
    return r;
}

RexxMethod3(RexxObjectPtr, LuaObj_Unknown, CSTRING, message, RexxArrayObject, args, CSELF, self)
{
    return callField(context, self, message, args, -1, false);
}

// o~method(name, args...): o:name(...)   o~func(name, args...): o.name(...)
RexxMethod3(RexxObjectPtr, LuaObj_Method, CSTRING, name, ARGLIST, args, CSELF, self)
{
    RexxArrayObject rest = context->ArraySize(args) > 1
        ? (RexxArrayObject)context->SendMessage2(args, "SECTION", context->WholeNumber(2), context->WholeNumber(context->ArraySize(args) - 1))
        : context->NewArray(0);
    return callField(context, self, name, rest, 1, false);
}

RexxMethod3(RexxObjectPtr, LuaObj_Func, CSTRING, name, ARGLIST, args, CSELF, self)
{
    RexxArrayObject rest = context->ArraySize(args) > 1
        ? (RexxArrayObject)context->SendMessage2(args, "SECTION", context->WholeNumber(2), context->WholeNumber(context->ArraySize(args) - 1))
        : context->NewArray(0);
    return callField(context, self, name, rest, 0, false);
}

// o~call(args...): o(...), first result. o~callAll(args...): all results, as an Array
static RexxObjectPtr callSelf(RexxMethodContext *context, POINTER self, RexxArrayObject args, bool all)
{
    RexxThreadContext *c = context->threadContext;
    cur = c; ensureLua();
    pushSelf(c, self);
    int nargs; pushArgs(c, args, nargs);
    int n = pcallAll(c, nargs);
    return all ? allResults(c, n) : firstResult(c, n);
}

RexxMethod2(RexxObjectPtr, LuaObj_Call, ARGLIST, args, CSELF, self)    { return callSelf(context, self, args, false); }
RexxMethod2(RexxObjectPtr, LuaObj_CallAll, ARGLIST, args, CSELF, self) { return callSelf(context, self, args, true); }

// o~get(name): the field (any case), never called
RexxMethod2(RexxObjectPtr, LuaObj_Get, CSTRING, name, CSELF, self)
{
    RexxThreadContext *c = context->threadContext;
    cur = c; ensureLua();
    pushSelf(c, self);
    resolveName(name);
    RexxObjectPtr r = toRexx(c, -1);
    lua_pop(L, 2);
    return r;
}

// o[k], o[k]=v: raw Lua indexing (keys: canonical integers become integers)
RexxMethod2(RexxObjectPtr, LuaObj_At, RexxObjectPtr, key, CSELF, self)
{
    RexxThreadContext *c = context->threadContext;
    cur = c; ensureLua();
    pushSelf(c, self);
    pushValue(c, key, true);
    lua_gettable(L, -2);
    RexxObjectPtr r = toRexx(c, -1);
    lua_pop(L, 2);
    return r;
}

RexxMethod3(RexxObjectPtr, LuaObj_Put, RexxObjectPtr, value, RexxObjectPtr, key, CSELF, self)
{
    RexxThreadContext *c = context->threadContext;
    cur = c; ensureLua();
    pushSelf(c, self);
    pushValue(c, key, true);
    pushValue(c, value);
    lua_settable(L, -3);
    lua_pop(L, 1);
    return NULLOBJECT;
}

RexxMethod1(RexxObjectPtr, LuaObj_String, CSELF, self)
{
    RexxThreadContext *c = context->threadContext;
    cur = c; ensureLua();
    pushSelf(c, self);
    size_t len;
    const char *s = luaL_tolstring(L, -1, &len);
    RexxObjectPtr r = c->NewString(s, len);
    lua_pop(L, 2);
    return r;
}

// do x over o: the sequence part (1..#o)
RexxMethod1(RexxObjectPtr, LuaObj_MakeArray, CSELF, self)
{
    RexxThreadContext *c = context->threadContext;
    cur = c; ensureLua();
    pushSelf(c, self);
    lua_Integer n = luaL_len(L, -1);
    RexxArrayObject a = c->NewArray((size_t)n);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, -1, i);
        c->ArrayPut(a, toRexx(c, -1), (size_t)i);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return a;
}

RexxMethod1(RexxObjectPtr, LuaObj_Type, CSELF, self)
{
    cur = context->threadContext; ensureLua();
    pushSelf(context->threadContext, self);
    RexxObjectPtr r = context->String(luaL_typename(L, -1));
    lua_pop(L, 1);
    return r;
}

RexxMethod1(RexxObjectPtr, LuaObj_Release, CSELF, self)
{
    if (L && self) luaL_unref(L, LUA_REGISTRYINDEX, (int)(intptr_t)self);
    return NULLOBJECT;
}

/* ------------------------------------------------------------------------ */
/* Package                                                                    */
/* ------------------------------------------------------------------------ */

RexxMethodEntry rexxlua_methods[] = {
    REXX_METHOD(Lua_Eval,         Lua_Eval),
    REXX_METHOD(Lua_Global,       Lua_Global),
    REXX_METHOD(Lua_Require,      Lua_Require),
    REXX_METHOD(Lua_Boolean,      Lua_Boolean),
    REXX_METHOD(Lua_Table,        Lua_Table),
    REXX_METHOD(Lua_Predicate,    Lua_Predicate),
    REXX_METHOD(LuaObj_Unknown,   LuaObj_Unknown),
    REXX_METHOD(LuaObj_Method,    LuaObj_Method),
    REXX_METHOD(LuaObj_Func,      LuaObj_Func),
    REXX_METHOD(LuaObj_Call,      LuaObj_Call),
    REXX_METHOD(LuaObj_CallAll,   LuaObj_CallAll),
    REXX_METHOD(LuaObj_Get,       LuaObj_Get),
    REXX_METHOD(LuaObj_At,        LuaObj_At),
    REXX_METHOD(LuaObj_Put,       LuaObj_Put),
    REXX_METHOD(LuaObj_String,    LuaObj_String),
    REXX_METHOD(LuaObj_MakeArray, LuaObj_MakeArray),
    REXX_METHOD(LuaObj_Type,      LuaObj_Type),
    REXX_METHOD(LuaObj_Release,   LuaObj_Release),
    REXX_LAST_METHOD()
};

RexxPackageEntry rexxlua_package_entry = {
    STANDARD_PACKAGE_HEADER
    REXX_INTERPRETER_5_0_0,
    "rexxlua",
    "0.0.1",
    NULL, NULL,
    NULL,
    rexxlua_methods
};

OOREXX_GET_PACKAGE(rexxlua);
