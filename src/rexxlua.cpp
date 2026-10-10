/*
 * rexxlua.cpp -- librexxlua.so: Lua embedded in ooRexx, as a native package.
 * Rexx side: lua.cls (.lua, .LuaState, .LuaObject).  How it works and why:
 * docs/design.md (the comments name its sections, e.g. *States*).
 *
 * Values, Lua -> Rexx:  nil -> .nil; boolean -> 1/0; integer -> "%lld";
 *   float -> Lua's own tostring; string -> string (bytes, unchanged);
 *   a Rexx object passed in earlier -> that object; anything else -> a
 *   .LuaObject proxy.
 * Values, Rexx -> Lua:  .nil -> nil; .LuaObject -> its value; Array -> a new
 *   sequence table (copy); string -> string (Lua coerces strings to numbers
 *   in arithmetic); Routine -> a Lua function; any other object -> userdata
 *   "RexxObject" that Lua can call or send messages to (obj:name(...)).
 *   Keys of [] and []= follow the canonical-integer rule instead: "1" -> 1.
 */

#include <oorexxapi.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <strings.h>

static const char *REXXOBJ = "RexxObject";          // metatable of Rexx objects in Lua
static const char *CACHE   = "rexxlua.cache";       // registry: Rexx object -> its userdata (weak)

/* ------------------------------------------------------------------------ */
/* States, proxies, locks (docs/design.md, *States* and *Threads*)           */
/* ------------------------------------------------------------------------ */

enum Kind { KIND_STATE = 1, KIND_PROXY = 2 };

// One per .LuaState.  Never freed: proxies and the graveyard may still point
// to it after it is closed (they then see `closed`).  A closed state keeps
// only this struct and an empty IdentityTable.
struct State {
    int kind = KIND_STATE;
    lua_State *L = NULL;
    std::recursive_mutex lock;              // held for a whole entry, callbacks included
    RexxThreadContext *tc = NULL;           // the thread inside, while locked
    RexxObjectPtr self = NULLOBJECT;        // its .LuaState, while locked
    RexxObjectPtr keep = NULLOBJECT;        // IdentityTable: Rexx object -> count (*Lifetime*)
    bool closed = false;
};

// One per .LuaObject: a Lua value in a state's registry.
struct Proxy {
    int kind = KIND_PROXY;
    State *st;
    int ref;
};

static State *stateOf(lua_State *L) { return *(State **)lua_getextraspace(L); }

// Entering a state: its lock, and who is inside (restored on exit, so that
// entries nest: Rexx -> Lua -> Rexx -> Lua on one thread).
struct Enter {
    State *st;
    std::unique_lock<std::recursive_mutex> lk;
    RexxThreadContext *savedTc;
    RexxObjectPtr savedSelf;
    bool owns;
    Enter(State *s, RexxThreadContext *tc, RexxObjectPtr self, bool tryOnly = false)
        : st(s), lk(s->lock, std::defer_lock)
    {
        owns = tryOnly ? lk.try_lock() : (lk.lock(), true);
        savedTc = st->tc; savedSelf = st->self;
        if (owns) { st->tc = tc; if (self != NULLOBJECT) st->self = self; }
    }
    ~Enter() { if (owns) { st->tc = savedTc; st->self = savedSelf; } }
};

static void raise(RexxThreadContext *c, const std::string &m)
{
    c->RaiseException1(Rexx_Error_Execution_user_defined, c->String(m.c_str()));
}

/* ------------------------------------------------------------------------ */
/* The graveyard (docs/design.md, *Lifetime*)                                 */
/* ------------------------------------------------------------------------ */

/*
 * The uninits of .LuaObject and .LuaState are Rexx code (lua.cls) that only
 * queue their pointer on .LuaObject~graveyard: a native uninit releases the
 * kernel lock inside ooRexx's runUninits loop, which crashes when other
 * threads create objects meanwhile (ooRexx bug #2101, fixed in r13256).  The
 * next native method drains it: a proxy's registry reference is released, a
 * state is closed.  A state another thread is using is left for later
 * (try_lock), so that draining never waits for a lock.
 *
 * One Queue for the whole process, kept by a global reference (as in
 * ooRexx/Python: a package loaded afresh gets a new .LuaObject class).
 */
static RexxObjectPtr graveyard = NULLOBJECT;
static std::mutex graveyardLock;

static void closeState(RexxThreadContext *c, State *st);

static void drain(RexxThreadContext *c)
{
    if (graveyard == NULLOBJECT) return;
    std::vector<void *> later;
    for (;;) {
        RexxObjectPtr p = c->SendMessage0(graveyard, "PULL");
        if (p == NULLOBJECT || p == c->Nil()) break;
        void *v = c->PointerValue((RexxPointerObject)p);
        if (v == NULL) continue;
        if (*(int *)v == KIND_PROXY) {
            Proxy *px = (Proxy *)v;
            if (px->st->closed) { delete px; continue; }
            Enter e(px->st, c, NULLOBJECT, true);
            if (!e.owns) { later.push_back(v); continue; }
            if (!px->st->closed) luaL_unref(px->st->L, LUA_REGISTRYINDEX, px->ref);
            delete px;
        }
        else {
            State *st = (State *)v;
            Enter e(st, c, NULLOBJECT, true);
            if (!e.owns) { later.push_back(v); continue; }
            closeState(c, st);
        }
    }
    for (void *v : later) c->SendMessage1(graveyard, "QUEUE", c->NewPointer(v));
}

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

static RexxObjectPtr toRexx(State *st, int idx);
static bool pushValue(State *st, RexxObjectPtr v, bool asKey = false);

static int msgh(lua_State *L)
{
    const char *m = lua_tostring(L, 1);
    if (m == NULL) m = luaL_tolstring(L, 1, NULL);
    luaL_traceback(L, L, m, 1);
    return 1;
}

/* Run the function and nargs arguments on top of the stack, protected.
 * Leaves all results on the stack and returns their count, or -1 after
 * raising a Rexx SYNTAX 98.900 with Lua's message and traceback.          */
static int pcallAll(State *st, int nargs)
{
    lua_State *L = st->L;
    int base = lua_gettop(L) - nargs;       // index of the function
    lua_pushcfunction(L, msgh);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, LUA_MULTRET, base);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        std::string m = "Lua error: ";
        m += lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error object)";
        lua_pop(L, 1);
        raise(st->tc, m);
        return -1;
    }
    return lua_gettop(L) - base + 1;
}

/* First result as a Rexx object (.nil if none); pops all results. */
static RexxObjectPtr firstResult(State *st, int n)
{
    if (n < 0) return NULLOBJECT;
    RexxObjectPtr r = n == 0 ? st->tc->Nil() : toRexx(st, -n);
    lua_pop(st->L, n);
    return r;
}

/* All results as a Rexx Array; pops them. */
static RexxObjectPtr allResults(State *st, int n)
{
    if (n < 0) return NULLOBJECT;
    RexxThreadContext *c = st->tc;
    RexxArrayObject a = c->NewArray(n);
    for (int i = 1; i <= n; i++) c->ArrayPut(a, toRexx(st, -n + i - 1), i);
    lua_pop(st->L, n);
    return a;
}

/* ------------------------------------------------------------------------ */
/* Rexx objects inside Lua: userdata "RexxObject" (docs/design.md, *Lifetime*) */
/* ------------------------------------------------------------------------ */

/*
 * A Rexx object Lua holds is kept alive by the state's IdentityTable `keep`
 * (object -> count), not by a global reference: ReleaseGlobalReference does
 * not release in released ooRexx 5.x (bug #2100).  One userdata per object
 * (CACHE, weak values), so that identity holds in Lua.  The count is needed
 * because Lua clears a weak entry before running the old userdata's __gc:
 * the object can get a new userdata while the old one still waits for it.
 */
struct RexxBox { RexxObjectPtr obj; };

static void keepAdd(State *st, RexxObjectPtr obj)
{
    RexxThreadContext *c = st->tc;
    RexxObjectPtr n = c->SendMessage1(st->keep, "AT", obj);
    wholenumber_t k = 0;
    if (n != NULLOBJECT && n != c->Nil()) c->WholeNumber(n, &k);
    c->SendMessage2(st->keep, "PUT", c->WholeNumber(k + 1), obj);
}

static void keepRemove(State *st, RexxObjectPtr obj)
{
    RexxThreadContext *c = st->tc;
    if (c == NULL) return;
    RexxObjectPtr n = c->SendMessage1(st->keep, "AT", obj);
    wholenumber_t k = 0;
    if (n != NULLOBJECT && n != c->Nil()) c->WholeNumber(n, &k);
    if (k <= 1) c->SendMessage1(st->keep, "REMOVE", obj);
    else c->SendMessage2(st->keep, "PUT", c->WholeNumber(k - 1), obj);
}

static int rexxCallBack(lua_State *L, RexxObjectPtr target, const char *message, int first)
{
    State *st = stateOf(L);
    RexxThreadContext *c = st->tc;
    int n = lua_gettop(L);
    RexxArrayObject args = c->NewArray(n >= first ? n - first + 1 : 0);
    for (int i = first; i <= n; i++) {
        RexxObjectPtr v = toRexx(st, i);
        if (v != c->Nil()) c->ArrayPut(args, v, i - first + 1);
    }
    RexxObjectPtr r;
    if (message == NULL && c->IsRoutine(target))
        r = c->CallRoutine((RexxRoutineObject)target, args);
    else
        r = c->SendMessage(target, message ? message : "CALL", args);
    if (c->CheckCondition()) {
        {   // no C++ object may live across lua_error (a longjmp)
            RexxCondition cd;
            c->DecodeConditionInfo(c->GetConditionInfo(), &cd);
            c->ClearCondition();
            std::string m = "Rexx error " + std::to_string(cd.code / 1000) + "." + std::to_string(cd.code % 1000);
            if (cd.message) { m += ": "; m += c->CString(cd.message); }
            else if (cd.errortext) { m += ": "; m += c->CString(cd.errortext); }
            lua_pushstring(L, m.c_str());
        }
        return lua_error(L);
    }
    if (r == NULLOBJECT) return 0;
    if (!pushValue(st, r)) return lua_error(L);
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

static void pushBox(State *st, RexxObjectPtr v)
{
    lua_State *L = st->L;
    lua_getfield(L, LUA_REGISTRYINDEX, CACHE);
    lua_rawgetp(L, -1, v);
    if (!lua_isnil(L, -1)) { lua_remove(L, -2); return; }   // the object's userdata
    lua_pop(L, 1);
    RexxBox *b = (RexxBox *)lua_newuserdatauv(L, sizeof(RexxBox), 0);
    b->obj = v;
    luaL_setmetatable(L, REXXOBJ);
    keepAdd(st, v);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, -3, v);
    lua_remove(L, -2);
}

static void pushRoutine(State *st, RexxObjectPtr v, bool predicate)
{
    pushBox(st, v);
    lua_pushboolean(st->L, predicate);
    lua_pushcclosure(st->L, routine_closure, 2);
}

static int box_tostring(lua_State *L)
{
    RexxBox *b = (RexxBox *)luaL_checkudata(L, 1, REXXOBJ);
    RexxThreadContext *c = stateOf(L)->tc;
    RexxStringObject s = c->ObjectToString(b->obj);
    lua_pushlstring(L, c->StringData(s), c->StringLength(s));
    return 1;
}

static int box_gc(lua_State *L)
{
    RexxBox *b = (RexxBox *)luaL_checkudata(L, 1, REXXOBJ);
    if (b->obj) keepRemove(stateOf(L), b->obj);
    b->obj = NULL;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Opening and closing a state                                                */
/* ------------------------------------------------------------------------ */

/*
 * C modules built for Lua (LuaRocks's) are not linked against liblua: they
 * expect the lua_* symbols to be global.  ooRexx loads this package with
 * RTLD_LOCAL, so the liblua it brought in is not; reopening it with
 * RTLD_NOLOAD | RTLD_GLOBAL makes it global (docs/design.md, *C modules*).
 */
static void makeLuaGlobal()
{
    static std::once_flag once;
    std::call_once(once, [] {
        Dl_info info;
        if (dladdr((void *)&lua_newstate, &info) && info.dli_fname)
            dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
    });
}

static State *openState(RexxThreadContext *c)
{
    makeLuaGlobal();
    State *st = new State();
    lua_State *L = luaL_newstate();
    if (L == NULL) { delete st; return NULL; }
    st->L = L;
    *(State **)lua_getextraspace(L) = st;
    RexxObjectPtr keep = c->SendMessage0(c->FindClass("IDENTITYTABLE"), "NEW");
    st->keep = c->RequestGlobalReference(keep);       // one per state, never released (#2100)
    luaL_openlibs(L);
    luaL_newmetatable(L, REXXOBJ);
    static const luaL_Reg mt[] = {
        {"__call", box_call}, {"__index", box_index},
        {"__tostring", box_tostring}, {"__gc", box_gc}, {NULL, NULL}};
    luaL_setfuncs(L, mt, 0);
    lua_pop(L, 1);
    lua_newtable(L);                                  // CACHE, weak values
    lua_newtable(L);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, CACHE);
    return st;
}

// With st entered.  lua_close runs the __gc of every Rexx object Lua held.
static void closeState(RexxThreadContext *c, State *st)
{
    if (st->closed) return;
    lua_close(st->L);
    st->L = NULL;
    st->closed = true;
    c->SendMessage0(st->keep, "EMPTY");
}

/* ------------------------------------------------------------------------ */
/* Conversions                                                                */
/* ------------------------------------------------------------------------ */

static RexxObjectPtr newProxy(State *st, int idx)
{
    RexxThreadContext *c = st->tc;
    lua_pushvalue(st->L, idx);
    Proxy *p = new Proxy();
    p->st = st;
    p->ref = luaL_ref(st->L, LUA_REGISTRYINDEX);
    return c->SendMessage2(c->FindClass("LUAOBJECT"), "NEW", c->NewPointer(p), st->self);
}

static RexxObjectPtr toRexx(State *st, int idx)
{
    RexxThreadContext *c = st->tc;
    lua_State *L = st->L;
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
        return newProxy(st, idx);
    }
    default:
        return newProxy(st, idx);
    }
}

// The Proxy of a .LuaObject (NULL if v is not one).
static Proxy *proxyOf(RexxThreadContext *c, RexxObjectPtr v)
{
    if (!c->IsOfType(v, "LUAOBJECT")) return NULL;
    RexxObjectPtr p = c->SendMessage0(v, "LUA.REF");
    return p == NULLOBJECT ? NULL : (Proxy *)c->PointerValue((RexxPointerObject)p);
}

/* Push a Rexx value.  False if it cannot go to this state (a .LuaObject of
 * another state, or of a closed one): then the error message is pushed
 * instead, for lua_error or for raiseTop.                                  */
static bool pushValue(State *st, RexxObjectPtr v, bool asKey)
{
    RexxThreadContext *c = st->tc;
    lua_State *L = st->L;
    if (v == NULLOBJECT || v == c->Nil()) { lua_pushnil(L); return true; }
    Proxy *p = proxyOf(c, v);
    if (p != NULL) {
        if (p->st != st) {
            lua_pushstring(L, p->st->closed ? "a Lua object of a closed LuaState"
                                            : "a Lua object of another LuaState");
            return false;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, p->ref);
        return true;
    }
    if (c->IsArray(v)) {
        RexxArrayObject a = (RexxArrayObject)v;
        size_t n = c->ArrayItems(a);
        size_t size = c->ArraySize(a);
        lua_createtable(L, (int)n, 0);
        for (size_t i = 1; i <= size; i++) {
            RexxObjectPtr item = c->ArrayAt(a, i);
            if (item == NULLOBJECT) continue;
            if (!pushValue(st, item)) { lua_remove(L, -2); return false; }
            lua_rawseti(L, -2, (lua_Integer)i);
        }
        return true;
    }
    if (c->IsString(v) || c->IsOfType(v, "STRING")) {   // Integers from literals too
        RexxStringObject s = c->ObjectToString(v);
        const char *d = c->StringData(s);
        size_t n = c->StringLength(s);
        if (asKey && isCanonicalInteger(d, n)) lua_pushinteger(L, (lua_Integer)strtoll(d, NULL, 10));
        else lua_pushlstring(L, d, n);
        return true;
    }
    // A Routine: a real Lua function. Any other object: a userdata Lua can
    // send messages to (obj:name(...)).
    if (c->IsRoutine(v)) { pushRoutine(st, v, false); return true; }
    pushBox(st, v);
    return true;
}

// After a failed pushValue: raise its message as a Rexx error and pop it.
static void raiseTop(State *st)
{
    std::string m = std::string("Lua error: ") + lua_tostring(st->L, -1);
    lua_pop(st->L, 1);
    raise(st->tc, m);
}

/* Case-insensitive name resolution on the value at the top of the stack.
 * Rexx uppercases names: try lowercase, then as written, then (tables only)
 * a scan of the keys. Pushes the value found (possibly nil).             */
static void resolveName(lua_State *L, const char *name)
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
/* Entering, from a native method                                             */
/* ------------------------------------------------------------------------ */

// Every native method starts here: drain the graveyard, then enter the state
// (if open).  `ok` is false after a Rexx error has been raised.
struct Entry {
    State *st;
    Enter enter;
    bool ok;
    Entry(RexxMethodContext *m, State *s, RexxObjectPtr stateObj)
        : st(s), enter((drain(m->threadContext), s), m->threadContext, stateObj), ok(true)
    {
        if (st->closed) { raise(m->threadContext, "Lua error: the LuaState is closed"); ok = false; }
    }
};

/* ------------------------------------------------------------------------ */
/* .LuaState                                                                  */
/* ------------------------------------------------------------------------ */

// .LuaState~new: its CSELF
RexxMethod0(RexxObjectPtr, LuaState_New)
{
    drain(context->threadContext);
    State *st = openState(context->threadContext);
    if (st == NULL) { raise(context->threadContext, "Lua error: cannot create a Lua state"); return NULLOBJECT; }
    return context->NewPointer(st);
}

// .LuaObject~graveyard (lua.cls, the class's activate): one Queue for the process
RexxMethod0(RexxObjectPtr, Lua_Graveyard)
{
    std::lock_guard<std::mutex> guard(graveyardLock);
    if (graveyard == NULLOBJECT) {
        RexxObjectPtr q = context->SendMessage0(context->FindClass("QUEUE"), "NEW");
        if (q != NULLOBJECT) graveyard = context->RequestGlobalReference(q);
    }
    return graveyard != NULLOBJECT ? graveyard : context->Nil();
}

RexxMethod1(RexxObjectPtr, LuaState_Close, CSELF, self)
{
    State *st = (State *)self;
    drain(context->threadContext);
    Enter e(st, context->threadContext, context->GetSelf());
    closeState(context->threadContext, st);
    return NULLOBJECT;
}

RexxMethod1(logical_t, LuaState_IsClosed, CSELF, self)
{
    return ((State *)self)->closed;
}

// ~eval(code, args...): "return code" if it compiles, else code as a chunk
RexxMethod3(RexxObjectPtr, LuaState_Eval, CSTRING, code, ARGLIST, args, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    State *st = en.st; lua_State *L = st->L;
    int top = lua_gettop(L);
    std::string ret = std::string("return ") + code;
    if (luaL_loadbuffer(L, ret.c_str(), ret.size(), "=eval") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, code, strlen(code), "=eval") != LUA_OK) {
            raiseTop(st);
            return NULLOBJECT;
        }
    }
    size_t size = context->ArraySize(args);
    for (size_t i = 2; i <= size; i++)
        if (!pushValue(st, context->ArrayAt(args, i))) {
            raiseTop(st);
            lua_settop(L, top);
            return NULLOBJECT;
        }
    int nargs = size > 1 ? (int)size - 1 : 0;
    return firstResult(st, pcallAll(st, nargs));
}

// ~global(name)
RexxMethod2(RexxObjectPtr, LuaState_Global, CSTRING, name, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    lua_getglobal(en.st->L, name);
    RexxObjectPtr r = toRexx(en.st, -1);
    lua_pop(en.st->L, 1);
    return r;
}

// ~require(name)
RexxMethod2(RexxObjectPtr, LuaState_Require, CSTRING, name, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    lua_getglobal(en.st->L, "require");
    lua_pushstring(en.st->L, name);
    return firstResult(en.st, pcallAll(en.st, 1));
}

// ~true, ~false
RexxMethod2(RexxObjectPtr, LuaState_Boolean, logical_t, b, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    lua_pushboolean(en.st->L, b ? 1 : 0);
    RexxObjectPtr r = newProxy(en.st, -1);
    lua_pop(en.st->L, 1);
    return r;
}

// ~predicate(routine): a Lua function whose Rexx 1/0 result is a boolean
RexxMethod2(RexxObjectPtr, LuaState_Predicate, RexxObjectPtr, routine, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    pushRoutine(en.st, routine, true);
    RexxObjectPtr r = newProxy(en.st, -1);
    lua_pop(en.st->L, 1);
    return r;
}

// ~table(array | directory | stringtable): a new Lua table (copy)
RexxMethod2(RexxObjectPtr, LuaState_Table, OPTIONAL_RexxObjectPtr, src, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    State *st = en.st; lua_State *L = st->L;
    RexxThreadContext *c = context->threadContext;
    int top = lua_gettop(L);
    if (src == NULLOBJECT) lua_newtable(L);
    else if (c->IsArray(src)) {
        if (!pushValue(st, src)) { raiseTop(st); return NULLOBJECT; }
    }
    else if (c->IsDirectory(src) || c->IsStringTable(src)) {
        lua_newtable(L);
        RexxObjectPtr sup = c->SendMessage0(src, "SUPPLIER");
        while (c->SupplierAvailable((RexxSupplierObject)sup)) {
            if (!pushValue(st, c->SupplierIndex((RexxSupplierObject)sup), true) ||
                !pushValue(st, c->SupplierItem((RexxSupplierObject)sup))) {
                raiseTop(st); lua_settop(L, top); return NULLOBJECT;
            }
            lua_settable(L, -3);
            c->SupplierNext((RexxSupplierObject)sup);
        }
    }
    else lua_newtable(L);
    RexxObjectPtr r = newProxy(st, -1);
    lua_pop(L, 1);
    return r;
}

// ~"LUA.STATS", for the tests: "refs kept" -- the registry references held
// by .LuaObjects (integer keys past Lua's own, whose values are not the
// numbers of luaL_ref's free list), and the Rexx objects Lua keeps alive
RexxMethod1(RexxObjectPtr, LuaState_Stats, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    lua_State *L = en.st->L;
    long refs = 0;
    lua_pushnil(L);
    while (lua_next(L, LUA_REGISTRYINDEX)) {
        if (lua_isinteger(L, -2) && lua_tointeger(L, -2) > LUA_RIDX_LAST && lua_type(L, -1) != LUA_TNUMBER) refs++;
        lua_pop(L, 1);
    }
    RexxObjectPtr kept = context->SendMessage0(en.st->keep, "ITEMS");
    std::string r = std::to_string(refs) + " " + context->CString(context->ObjectToString(kept));
    return context->String(r.c_str());
}

/* ------------------------------------------------------------------------ */
/* .LuaObject                                                                 */
/* ------------------------------------------------------------------------ */

// Entering from a .LuaObject method: its state, and its .LuaState object
#define PROXY_ENTRY                                                         \
    Proxy *px = (Proxy *)self;                                              \
    Entry en(context, px->st, context->GetObjectVariable("STATE"));          \
    if (!en.ok) return NULLOBJECT;                                          \
    State *st = en.st; lua_State *L = st->L;                                \
    RexxThreadContext *c = context->threadContext;                          \
    (void)L; (void)c;

static void pushSelf(Proxy *px) { lua_rawgeti(px->st->L, LUA_REGISTRYINDEX, px->ref); }

// o~name(args...): the field (any case); if a function, called.
//   SPIKE HEURISTIC for the colon problem: when o has a metatable the call is
//   o:name(...) (self passed), otherwise o.name(...). ~method and ~func force.
// o~name=v: sets the field.
static RexxObjectPtr callField(RexxMethodContext *context, Proxy *px, CSTRING message,
                               RexxArrayObject args, int colon /* -1 auto, 0, 1 */, bool all)
{
    State *st = px->st; lua_State *L = st->L;
    RexxThreadContext *c = context->threadContext;
    std::string name = message;
    size_t size = args ? c->ArraySize(args) : 0;
    int top = lua_gettop(L);
    pushSelf(px);
    int obj = lua_gettop(L);
    if (name.size() > 1 && name.back() == '=' && colon == -1) {      // assignment
        name.pop_back();
        std::string lower(name);
        for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
        lua_getfield(L, obj, name.c_str());                         // keep an existing case
        const char *key = lua_isnil(L, -1) ? lower.c_str() : name.c_str();
        lua_pop(L, 1);
        if (!pushValue(st, size ? c->ArrayAt(args, 1) : NULLOBJECT)) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
        lua_setfield(L, obj, key);
        lua_settop(L, top);
        return NULLOBJECT;
    }
    resolveName(L, name.c_str());
    bool callable = lua_type(L, -1) == LUA_TFUNCTION;
    if (!callable && luaL_getmetafield(L, -1, "__call") != LUA_TNIL) { lua_pop(L, 1); callable = true; }
    if (!callable) {
        if (size > 0) {
            lua_settop(L, top);
            raise(c, "Lua error: field '" + name + "' is not callable");
            return NULLOBJECT;
        }
        RexxObjectPtr r = toRexx(st, -1);
        lua_settop(L, top);
        return r;
    }
    if (colon == -1) {
        colon = lua_getmetatable(L, obj) ? 1 : 0;
        if (colon) lua_pop(L, 1);
    }
    int nargs = 0;
    if (colon) { lua_pushvalue(L, obj); nargs = 1; }
    for (size_t i = 1; i <= size; i++)
        if (!pushValue(st, c->ArrayAt(args, i))) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
    nargs += (int)size;
    int n = pcallAll(st, nargs);
    RexxObjectPtr r = all ? allResults(st, n) : firstResult(st, n);
    lua_settop(L, top);
    return r;
}

RexxMethod3(RexxObjectPtr, LuaObj_Unknown, CSTRING, message, RexxArrayObject, args, CSELF, self)
{
    PROXY_ENTRY
    return callField(context, px, message, args, -1, false);
}

static RexxArrayObject rest(RexxMethodContext *context, RexxArrayObject args)
{
    return context->ArraySize(args) > 1
        ? (RexxArrayObject)context->SendMessage2(args, "SECTION", context->WholeNumber(2), context->WholeNumber(context->ArraySize(args) - 1))
        : context->NewArray(0);
}

// o~method(name, args...): o:name(...)   o~func(name, args...): o.name(...)
RexxMethod3(RexxObjectPtr, LuaObj_Method, CSTRING, name, ARGLIST, args, CSELF, self)
{
    PROXY_ENTRY
    return callField(context, px, name, rest(context, args), 1, false);
}

RexxMethod3(RexxObjectPtr, LuaObj_Func, CSTRING, name, ARGLIST, args, CSELF, self)
{
    PROXY_ENTRY
    return callField(context, px, name, rest(context, args), 0, false);
}

// o~call(args...): o(...), first result. o~callAll(args...): all results, as an Array
static RexxObjectPtr callSelf(RexxMethodContext *context, Proxy *px, RexxArrayObject args, bool all)
{
    State *st = px->st; lua_State *L = st->L;
    RexxThreadContext *c = context->threadContext;
    int top = lua_gettop(L);
    pushSelf(px);
    size_t size = args ? c->ArraySize(args) : 0;
    for (size_t i = 1; i <= size; i++)
        if (!pushValue(st, c->ArrayAt(args, i))) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
    int n = pcallAll(st, (int)size);
    return all ? allResults(st, n) : firstResult(st, n);
}

RexxMethod2(RexxObjectPtr, LuaObj_Call, ARGLIST, args, CSELF, self)    { PROXY_ENTRY return callSelf(context, px, args, false); }
RexxMethod2(RexxObjectPtr, LuaObj_CallAll, ARGLIST, args, CSELF, self) { PROXY_ENTRY return callSelf(context, px, args, true); }

// o~get(name): the field (any case), never called
RexxMethod2(RexxObjectPtr, LuaObj_Get, CSTRING, name, CSELF, self)
{
    PROXY_ENTRY
    pushSelf(px);
    resolveName(L, name);
    RexxObjectPtr r = toRexx(st, -1);
    lua_pop(L, 2);
    return r;
}

// o[k], o[k]=v: raw Lua indexing (keys: canonical integers become integers)
RexxMethod2(RexxObjectPtr, LuaObj_At, RexxObjectPtr, key, CSELF, self)
{
    PROXY_ENTRY
    int top = lua_gettop(L);
    pushSelf(px);
    if (!pushValue(st, key, true)) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
    lua_gettable(L, -2);
    RexxObjectPtr r = toRexx(st, -1);
    lua_settop(L, top);
    return r;
}

RexxMethod3(RexxObjectPtr, LuaObj_Put, RexxObjectPtr, value, RexxObjectPtr, key, CSELF, self)
{
    PROXY_ENTRY
    int top = lua_gettop(L);
    pushSelf(px);
    if (!pushValue(st, key, true) || !pushValue(st, value)) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
    lua_settable(L, -3);
    lua_settop(L, top);
    return NULLOBJECT;
}

RexxMethod1(RexxObjectPtr, LuaObj_String, CSELF, self)
{
    PROXY_ENTRY
    pushSelf(px);
    size_t len;
    const char *s = luaL_tolstring(L, -1, &len);
    RexxObjectPtr r = c->NewString(s, len);
    lua_pop(L, 2);
    return r;
}

// do x over o: the sequence part (1..#o)
RexxMethod1(RexxObjectPtr, LuaObj_MakeArray, CSELF, self)
{
    PROXY_ENTRY
    pushSelf(px);
    lua_Integer n = luaL_len(L, -1);
    RexxArrayObject a = c->NewArray((size_t)n);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, -1, i);
        c->ArrayPut(a, toRexx(st, -1), (size_t)i);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return a;
}

RexxMethod1(RexxObjectPtr, LuaObj_Type, CSELF, self)
{
    PROXY_ENTRY
    pushSelf(px);
    RexxObjectPtr r = c->String(luaL_typename(L, -1));
    lua_pop(L, 1);
    return r;
}

/* ------------------------------------------------------------------------ */
/* Package                                                                    */
/* ------------------------------------------------------------------------ */

RexxMethodEntry rexxlua_methods[] = {
    REXX_METHOD(LuaState_New,       LuaState_New),
    REXX_METHOD(Lua_Graveyard,      Lua_Graveyard),
    REXX_METHOD(LuaState_Close,     LuaState_Close),
    REXX_METHOD(LuaState_IsClosed,  LuaState_IsClosed),
    REXX_METHOD(LuaState_Eval,      LuaState_Eval),
    REXX_METHOD(LuaState_Global,    LuaState_Global),
    REXX_METHOD(LuaState_Require,   LuaState_Require),
    REXX_METHOD(LuaState_Boolean,   LuaState_Boolean),
    REXX_METHOD(LuaState_Table,     LuaState_Table),
    REXX_METHOD(LuaState_Predicate, LuaState_Predicate),
    REXX_METHOD(LuaState_Stats,     LuaState_Stats),
    REXX_METHOD(LuaObj_Unknown,     LuaObj_Unknown),
    REXX_METHOD(LuaObj_Method,      LuaObj_Method),
    REXX_METHOD(LuaObj_Func,        LuaObj_Func),
    REXX_METHOD(LuaObj_Call,        LuaObj_Call),
    REXX_METHOD(LuaObj_CallAll,     LuaObj_CallAll),
    REXX_METHOD(LuaObj_Get,         LuaObj_Get),
    REXX_METHOD(LuaObj_At,          LuaObj_At),
    REXX_METHOD(LuaObj_Put,         LuaObj_Put),
    REXX_METHOD(LuaObj_String,      LuaObj_String),
    REXX_METHOD(LuaObj_MakeArray,   LuaObj_MakeArray),
    REXX_METHOD(LuaObj_Type,        LuaObj_Type),
    REXX_LAST_METHOD()
};

RexxPackageEntry rexxlua_package_entry = {
    STANDARD_PACKAGE_HEADER
    REXX_INTERPRETER_5_0_0,
    "rexxlua",
    "0.1.0",
    NULL, NULL,
    NULL,
    rexxlua_methods
};

OOREXX_GET_PACKAGE(rexxlua);
