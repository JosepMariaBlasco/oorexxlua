/*
 * rexxlua.cpp -- librexxlua.so: Lua embedded in ooRexx, as a native package.
 * Rexx side: lua.cls (.lua, .LuaState, .LuaObject).  How it works and why:
 * docs/design.md (the comments name its sections, e.g. *States*).
 *
 * Values (docs/design.md, *Values*), Lua -> Rexx: nil -> .nil; boolean ->
 *   1/0; integer -> its decimal string; float -> the shortest string that
 *   gives it back; string -> string; a Rexx object passed in earlier -> that
 *   object; anything else -> a .LuaObject proxy.
 * Rexx -> Lua: .nil -> nil; .LuaObject -> its value; Array -> a new sequence
 *   table (copy); a string -> integer, float or string by ooRexx/Python's
 *   rule (numberKind); Routine -> a Lua function; any other object ->
 *   userdata "RexxObject" that Lua can call or send messages to.
 */

#include <oorexxapi.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#if LUA_VERSION_NUM < 503
#error "the bridge needs Lua 5.3 or later (integers, lua_getextraspace)"
#endif
#if LUA_VERSION_NUM < 504                   // Lua 5.3: one user value, always
#define lua_newuserdatauv(L, size, nuvalue) lua_newuserdata(L, size)
#endif
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cerrno>
#include <cstdlib>
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
    lua_State *L = NULL;                    // the Lua thread in use: main, or the
                                            // coroutine that called back into Rexx
    lua_State *main = NULL;                 // the state's main thread
    std::recursive_mutex lock;              // held for a whole entry, callbacks included
    RexxThreadContext *tc = NULL;           // the thread inside, while locked
    RexxObjectPtr self = NULLOBJECT;        // its .LuaState, while locked
    RexxObjectPtr keep = NULLOBJECT;        // IdentityTable: Rexx object -> count (*Lifetime*)
    int depth = 0;                          // entries in progress (nested ones too)
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
        if (owns) { st->tc = tc; if (self != NULLOBJECT) st->self = self; st->depth++; }
    }
    ~Enter() { if (owns) { st->depth--; st->tc = savedTc; st->self = savedSelf; } }
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
            // another thread's, or running on this one (a callback): later
            if (!e.owns || st->depth > 1) { later.push_back(v); continue; }
            closeState(c, st);
        }
    }
    for (void *v : later) c->SendMessage1(graveyard, "QUEUE", c->NewPointer(v));
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------ */

static bool isDigits(const char *p, const char *end)
{
    if (p == end) return false;
    for (; p < end; p++) if (*p < '0' || *p > '9') return false;
    return true;
}

// What a Rexx string becomes in Lua (docs/design.md, *Values*): the same rule
// as ooRexx/Python's, and the same code (its bridge.cpp, numberKind).
enum NumberKind { NOT_NUMBER, INTEGER_NUMBER, FLOAT_NUMBER };

static NumberKind numberKind(const char *s, size_t n)
{
    const char *p = s, *end = p + n;
    if (p < end && *p == '-') p++;
    if (p == end) return NOT_NUMBER;
    // canonical integer: 0, or no leading zero
    if (isDigits(p, end)) return (*p != '0' || end - p == 1) ? INTEGER_NUMBER : NOT_NUMBER;
    // a Rexx number with a decimal point and/or an exponent
    const char *e = p;
    while (e < end && *e != 'e' && *e != 'E') e++;
    const char *dot = (const char *)memchr(p, '.', e - p);
    bool mantissa;
    if (dot != NULL) {
        bool intPart = dot == p || isDigits(p, dot);
        bool fracPart = dot + 1 == e || isDigits(dot + 1, e);
        mantissa = intPart && fracPart && (e - p) > 1;      // not a lone "."
    }
    else mantissa = isDigits(p, e);
    if (!mantissa) return NOT_NUMBER;
    if (e == end) return dot != NULL ? FLOAT_NUMBER : NOT_NUMBER;   // "007": not a number
    const char *x = e + 1;
    if (x < end && (*x == '+' || *x == '-')) x++;
    return isDigits(x, end) ? FLOAT_NUMBER : NOT_NUMBER;
}

// Push a Rexx string by that rule.  An integer that does not fit in 64 bits
// stays a string (a float would lose digits silently).
static void pushRexxString(lua_State *L, const char *d, size_t n)
{
    std::string s(d, n);                    // NUL-terminated, for strtoll/strtod
    switch (numberKind(d, n)) {
    case INTEGER_NUMBER: {
        errno = 0;
        long long v = strtoll(s.c_str(), NULL, 10);
        if (errno == 0) { lua_pushinteger(L, (lua_Integer)v); return; }
        break;
    }
    case FLOAT_NUMBER:
        lua_pushnumber(L, (lua_Number)strtod(s.c_str(), NULL));
        return;
    default:
        break;
    }
    lua_pushlstring(L, d, n);
}

// A Lua float as Rexx sees it: the shortest of %.15g, %.16g, %.17g that gives
// the same double back, with ".0" when it looks like an integer (as Python's
// repr).  Lua's own %.14g loses digits: 0.1 + 0.2 would be "0.3".
static std::string formatFloat(double d)
{
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buf[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    std::string r(buf);
    if (r.find_first_of(".eE") == std::string::npos) r += ".0";
    return r;
}

static RexxObjectPtr toRexx(State *st, int idx);
static bool pushValue(State *st, RexxObjectPtr v);

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

/*
 * Protected operations (docs/design.md, *Errors*).  Getting a field, indexing,
 * tostring, # ... can run Lua code (metamethods), and a Lua error outside a
 * protected call aborts the whole process ("PANIC: unprotected error").  So
 * every such operation is a small C function run with lua_pcall: its error
 * becomes a Rexx condition.  These functions use the Lua API only (no C++
 * objects, which a Lua error, a longjmp, would not destroy).
 */
static bool protect(State *st, lua_CFunction f, int nargs)
{
    lua_pushcfunction(st->L, f);
    lua_insert(st->L, -(nargs + 1));
    return pcallAll(st, nargs) >= 0;
}

static void pushLower(lua_State *L, int idx)
{
    size_t n;
    const char *s = lua_tolstring(L, idx, &n);
    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, n);
    for (size_t i = 0; i < n; i++) p[i] = (char)tolower((unsigned char)s[i]);
    luaL_pushresultsize(&b, n);
}

// Is key (at k) a field of the object (at 1) itself, not through __index?
static int isRaw(lua_State *L, int k)
{
    if (lua_type(L, 1) != LUA_TTABLE) return 0;
    lua_pushvalue(L, k);
    int raw = lua_rawget(L, 1) != LUA_TNIL;
    lua_pop(L, 1);
    return raw;
}

/* p_resolve(obj, name) -> value, raw.  Rexx uppercases names (docs/design.md,
 * *Names*): try the name in lowercase, then as written, then (tables only) a
 * scan of the keys, ignoring case.  raw: found in the object itself.       */
static int p_resolve(lua_State *L)
{
    pushLower(L, 2);                                    // 3
    for (int k = 3; k >= 2; k--) {
        lua_pushvalue(L, k);
        if (lua_gettable(L, 1) != LUA_TNIL) { lua_pushboolean(L, isRaw(L, k)); return 2; }
        lua_pop(L, 1);
    }
    if (lua_type(L, 1) == LUA_TTABLE) {
        const char *name = lua_tostring(L, 2);
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            if (lua_type(L, -2) == LUA_TSTRING && strcasecmp(lua_tostring(L, -2), name) == 0) {
                lua_remove(L, -2);
                lua_pushboolean(L, 1);
                return 2;
            }
            lua_pop(L, 1);
        }
    }
    lua_pushnil(L);
    lua_pushboolean(L, 0);
    return 2;
}

/* p_assign(obj, name, value): obj[key] = value, where key is the existing
 * key that matches name ignoring case (as p_resolve finds it), or else the
 * name in lowercase.                                                       */
static int p_assign(lua_State *L)
{
    pushLower(L, 2);                                    // 4
    int key = 4;
    lua_pushvalue(L, 4);
    if (lua_gettable(L, 1) == LUA_TNIL) {
        lua_pushvalue(L, 2);
        if (lua_gettable(L, 1) != LUA_TNIL) key = 2;
        else if (lua_type(L, 1) == LUA_TTABLE) {
            const char *name = lua_tostring(L, 2);
            lua_pushnil(L);
            while (lua_next(L, 1)) {
                lua_pop(L, 1);
                if (lua_type(L, -1) == LUA_TSTRING && strcasecmp(lua_tostring(L, -1), name) == 0) {
                    key = lua_gettop(L);
                    break;
                }
            }
        }
    }
    lua_pushvalue(L, key);
    lua_pushvalue(L, 3);
    lua_settable(L, 1);
    return 0;
}

static int p_gettable(lua_State *L) { lua_gettable(L, 1); return 1; }          // obj, key -> obj[key]
static int p_settable(lua_State *L) { lua_settable(L, 1); return 0; }          // obj, key, value
static int p_tostring(lua_State *L) { luaL_tolstring(L, 1, NULL); return 1; }  // v -> tostring(v)
static int p_len(lua_State *L) { lua_pushinteger(L, luaL_len(L, 1)); return 1; }   // v -> #v
static int p_getglobal(lua_State *L) { lua_getglobal(L, lua_tostring(L, 1)); return 1; }
// op, a[, b] -> lua_arith(op): a op b, or op a for the unary ones
static int p_arith(lua_State *L)
{
    int op = (int)lua_tointeger(L, 1);
    lua_remove(L, 1);
    lua_arith(L, op);
    return 1;
}
// op, a, b -> lua_compare(a, b, op)
static int p_compare(lua_State *L)
{
    lua_pushboolean(L, lua_compare(L, 2, 3, (int)lua_tointeger(L, 1)));
    return 1;
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
struct RexxBox { RexxObjectPtr obj; };       // obj is NULL once its __gc has run

/*
 * Local references.  Every Rexx object the API returns is a local reference
 * of the native method in progress, freed only when it returns.  One eval
 * whose Lua code calls back into Rexx a million times would hold a million
 * argument arrays, so what the callbacks make is released as soon as it has
 * been used.  (The constants .nil, .true and .false are left alone.)
 */
static void release(RexxThreadContext *c, RexxObjectPtr o)
{
    if (o != NULLOBJECT && o != c->Nil() && o != c->True() && o != c->False())
        c->ReleaseLocalReference(o);
}

static void keepAdd(State *st, RexxObjectPtr obj)
{
    RexxThreadContext *c = st->tc;
    RexxObjectPtr n = c->SendMessage1(st->keep, "AT", obj);
    wholenumber_t k = 0;
    if (n != NULLOBJECT && n != c->Nil()) c->WholeNumber(n, &k);
    release(c, n);
    RexxObjectPtr count = c->WholeNumber(k + 1);
    c->SendMessage2(st->keep, "PUT", count, obj);
    release(c, count);
}

static void keepRemove(State *st, RexxObjectPtr obj)
{
    RexxThreadContext *c = st->tc;
    if (c == NULL) return;
    RexxObjectPtr n = c->SendMessage1(st->keep, "AT", obj);
    wholenumber_t k = 0;
    if (n != NULLOBJECT && n != c->Nil()) c->WholeNumber(n, &k);
    release(c, n);
    if (k <= 1) release(c, c->SendMessage1(st->keep, "REMOVE", obj));
    else {
        RexxObjectPtr count = c->WholeNumber(k - 1);
        c->SendMessage2(st->keep, "PUT", count, obj);
        release(c, count);
    }
}

// A Rexx condition, pending on this thread, as a Lua error (pushes the message;
// the caller then returns lua_error(L))
static void pushRexxCondition(lua_State *L, RexxThreadContext *c)
{
    RexxCondition cd;
    c->DecodeConditionInfo(c->GetConditionInfo(), &cd);
    c->ClearCondition();
    std::string m = "Rexx error " + std::to_string(cd.code / 1000) + "." + std::to_string(cd.code % 1000);
    if (cd.message) { m += ": "; m += c->CString(cd.message); }
    else if (cd.errortext) { m += ": "; m += c->CString(cd.errortext); }
    lua_pushstring(L, m.c_str());
}   // (the strings are gone before lua_error, a longjmp, runs)

// The string value of a Rexx object, or false with a condition pending (its
// `string` method can raise one: ObjectToString then returns NULL)
static bool stringValue(RexxThreadContext *c, RexxObjectPtr o, RexxStringObject &s)
{
    s = c->ObjectToString(o);
    return s != NULLOBJECT && !c->CheckCondition();
}

/*
 * The Lua C functions below run in some Lua thread L: the main one, or a
 * coroutine.  Everything the bridge does meanwhile (converting arguments,
 * pushing results, nested calls from Rexx back into Lua) must use that
 * thread's stack, so they set st->L = L on entry and put the previous one
 * back before every return and every lua_error (a longjmp: no RAII here).
 * docs/design.md, *Coroutines*.
 */
#define USE_THREAD(L)  State *st = stateOf(L); lua_State *outerL = st->L; st->L = (L)
#define END_THREAD()   (st->L = outerL)

// A Rexx object's userdata, refused once its __gc has run (a finalizer can
// bring a userdata back after that)
static RexxBox *checkBox(lua_State *L, int idx)
{
    RexxBox *b = (RexxBox *)luaL_checkudata(L, idx, REXXOBJ);
    if (b->obj == NULL) luaL_error(L, "a Rexx object used after Lua finalized it");
    return b;
}

static RexxBox *upvalueBox(lua_State *L)
{
    RexxBox *b = (RexxBox *)lua_touserdata(L, lua_upvalueindex(1));
    if (b->obj == NULL) luaL_error(L, "a Rexx object used after Lua finalized it");
    return b;
}

static int rexxCallBack(lua_State *L, RexxObjectPtr target, const char *message, int first)
{
    USE_THREAD(L);
    RexxThreadContext *c = st->tc;
    int n = lua_gettop(L);
    RexxArrayObject args = c->NewArray(n >= first ? n - first + 1 : 0);
    for (int i = first; i <= n; i++) {
        RexxObjectPtr v = toRexx(st, i);
        if (v != c->Nil()) c->ArrayPut(args, v, i - first + 1);
        release(c, v);                              // the Array holds it
    }
    RexxObjectPtr r;
    if (message == NULL && c->IsRoutine(target))
        r = c->CallRoutine((RexxRoutineObject)target, args);
    else
        r = c->SendMessage(target, message ? message : "CALL", args);
    release(c, args);
    if (c->CheckCondition()) {
        pushRexxCondition(L, c);
        END_THREAD();
        return lua_error(L);
    }
    if (r == NULLOBJECT) { END_THREAD(); return 0; }
    bool ok = pushValue(st, r);
    release(c, r);
    END_THREAD();
    if (!ok) return lua_error(L);
    return 1;
}

static int box_call(lua_State *L)       // rexxobj(...)
{
    RexxBox *b = checkBox(L, 1);
    return rexxCallBack(L, b->obj, NULL, 2);
}

static int box_method(lua_State *L)     // closure from __index: rexxobj:name(...)
{
    RexxBox *b = upvalueBox(L);
    const char *name = lua_tostring(L, lua_upvalueindex(2));
    // obj:name(...) passes obj first: skip it; obj.name(...) does not
    int first = (lua_touserdata(L, 1) == b) ? 2 : 1;
    return rexxCallBack(L, b->obj, name, first);
}

// Message names are uppercase in Rexx (pushes the name in uppercase)
static void pushUpper(lua_State *L, int idx, const char *suffix)
{
    size_t n;
    const char *s = lua_tolstring(L, idx, &n);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < n; i++) luaL_addchar(&b, (char)toupper((unsigned char)s[i]));
    luaL_addstring(&b, suffix);
    luaL_pushresult(&b);
}

/* Indexing a Rexx object (docs/design.md, *Rexx objects in Lua*).  In Lua,
 * obj.k and obj["k"] are the same thing, so: a string key is a message,
 * any other key goes to Rexx's [] and []=.                                */
static int box_index(lua_State *L)      // obj.name: the bound method; obj[k]: obj[k]
{
    RexxBox *b = checkBox(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_pushvalue(L, 1);
        pushUpper(L, 2, "");
        lua_pushcclosure(L, box_method, 2);
        return 1;
    }
    lua_settop(L, 2);
    return rexxCallBack(L, b->obj, "[]", 2);
}

static int box_newindex(lua_State *L)   // obj.name = v: NAME=; obj[k] = v: obj[k] = v
{
    RexxBox *b = checkBox(L, 1);
    lua_settop(L, 3);
    if (lua_type(L, 2) == LUA_TSTRING) {
        pushUpper(L, 2, "=");                               // 4: the message
        lua_pushvalue(L, 3);                                // 5: the value
        const char *msg = lua_tostring(L, 4);
        rexxCallBack(L, b->obj, msg, 5);
        return 0;
    }
    lua_pushvalue(L, 3);                                    // Rexx's []= takes value, index
    lua_pushvalue(L, 2);
    rexxCallBack(L, b->obj, "[]=", 4);
    return 0;
}

static int box_len(lua_State *L)        // #obj: obj~items
{
    RexxBox *b = checkBox(L, 1);
    lua_settop(L, 1);
    return rexxCallBack(L, b->obj, "ITEMS", 2);
}

// pairs(obj): through obj~supplier.  upvalue 1: the supplier, as a userdata.
static int pairs_next(lua_State *L)
{
    RexxBox *sup = upvalueBox(L);
    USE_THREAD(L);
    RexxThreadContext *c = st->tc;
    RexxSupplierObject sp = (RexxSupplierObject)sup->obj;
    if (!c->SupplierAvailable(sp)) {
        END_THREAD();
        if (c->CheckCondition()) { pushRexxCondition(L, c); return lua_error(L); }
        return 0;
    }
    RexxObjectPtr index = c->SupplierIndex(sp);
    RexxObjectPtr item = c->SupplierItem(sp);
    c->SupplierNext(sp);
    if (c->CheckCondition()) { END_THREAD(); pushRexxCondition(L, c); return lua_error(L); }
    bool ok = pushValue(st, index) && pushValue(st, item);
    release(c, index);
    release(c, item);
    END_THREAD();
    if (!ok) return lua_error(L);
    return 2;
}

static void pushBox(State *st, RexxObjectPtr v);

static int box_pairs(lua_State *L)
{
    RexxBox *b = checkBox(L, 1);
    USE_THREAD(L);
    RexxThreadContext *c = st->tc;
    RexxObjectPtr sup = c->SendMessage0(b->obj, "SUPPLIER");
    if (c->CheckCondition()) { END_THREAD(); pushRexxCondition(L, c); return lua_error(L); }
    if (sup == NULLOBJECT || !c->IsOfType(sup, "SUPPLIER")) {
        END_THREAD();
        return luaL_error(L, "pairs: the Rexx object's supplier is not a Supplier");
    }
    pushBox(st, sup);
    release(c, sup);                                // the userdata keeps it
    END_THREAD();
    lua_pushcclosure(L, pairs_next, 1);
    return 1;
}

// A Rexx Routine as a real Lua function (table.sort & co. check the type).
// upvalue 1: the box; upvalue 2: true for a predicate (Rexx 1/0 -> boolean)
static int routine_closure(lua_State *L)
{
    RexxBox *b = upvalueBox(L);
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
    RexxBox *b = checkBox(L, 1);
    RexxThreadContext *c = stateOf(L)->tc;
    RexxStringObject s;
    if (!stringValue(c, b->obj, s)) {
        if (c->CheckCondition()) pushRexxCondition(L, c);
        else lua_pushstring(L, "the Rexx object has no string value");
        return lua_error(L);
    }
    lua_pushlstring(L, c->StringData(s), c->StringLength(s));
    if ((RexxObjectPtr)s != b->obj) release(c, s);
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
    st->L = st->main = L;
    *(State **)lua_getextraspace(L) = st;
    RexxObjectPtr keep = c->SendMessage0(c->FindClass("IDENTITYTABLE"), "NEW");
    st->keep = c->RequestGlobalReference(keep);       // one per state, never released (#2100)
    luaL_openlibs(L);
    luaL_newmetatable(L, REXXOBJ);
    static const luaL_Reg mt[] = {
        {"__call", box_call}, {"__index", box_index}, {"__newindex", box_newindex},
        {"__len", box_len}, {"__pairs", box_pairs},
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
    lua_close(st->main);
    st->L = st->main = NULL;
    st->closed = true;
    c->SendMessage0(st->keep, "EMPTY");
}

/* ------------------------------------------------------------------------ */
/* Conversions                                                                */
/* ------------------------------------------------------------------------ */

static RexxObjectPtr newProxy(State *st, int idx)
{
    RexxThreadContext *c = st->tc;
    if (st->self == NULLOBJECT) return c->Nil();    // no .LuaState at hand (a __gc while drain closes it)
    lua_pushvalue(st->L, idx);
    Proxy *p = new Proxy();
    p->st = st;
    p->ref = luaL_ref(st->L, LUA_REGISTRYINDEX);
    RexxObjectPtr o = c->SendMessage2(c->FindClass("LUAOBJECT"), "NEW", c->NewPointer(p), st->self);
    if (o == NULLOBJECT) {                          // a condition is pending
        luaL_unref(st->L, LUA_REGISTRYINDEX, p->ref);
        delete p;
        return c->Nil();
    }
    return o;
}

static RexxObjectPtr toRexx(State *st, int idx)
{
    RexxThreadContext *c = st->tc;
    lua_State *L = st->L;
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:     return c->Nil();
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? c->True() : c->False();
    case LUA_TNUMBER: {
        if (lua_isinteger(L, idx)) {
            char buf[32];
            snprintf(buf, sizeof buf, "%lld", (long long)lua_tointeger(L, idx));
            return c->String(buf);
        }
        return c->String(formatFloat((double)lua_tonumber(L, idx)).c_str());
    }
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        return c->NewString(s, len);
    }
    case LUA_TUSERDATA: {
        RexxBox *b = (RexxBox *)luaL_testudata(L, idx, REXXOBJ);
        if (b) return b->obj != NULL ? b->obj : c->Nil();
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
static bool pushValue(State *st, RexxObjectPtr v)
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
        if (!lua_checkstack(L, 4)) { lua_pushstring(L, "an Array nested too deeply"); return false; }
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
        RexxStringObject s;
        if (!stringValue(c, v, s)) {
            if (c->CheckCondition()) pushRexxCondition(L, c);
            else lua_pushstring(L, "a Rexx string with no string value");
            return false;
        }
        pushRexxString(L, c->StringData(s), c->StringLength(s));
        if ((RexxObjectPtr)s != v) release(c, s);   // a new String (from a number): see release
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

// Room on the Lua stack for n more values (Lua guarantees only about 20, and
// does not check in release builds): false after raising a Rexx error
static bool room(State *st, int n)
{
    if (lua_checkstack(st->L, n + 4)) return true;
    raise(st->tc, "Lua error: too many arguments");
    return false;
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
    if (st->depth > 1) {        // from a callback: Lua code of this state is running
        raise(context->threadContext, "Lua error: a LuaState cannot be closed while Lua code in it is running");
        return NULLOBJECT;
    }
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
    if (!room(st, (int)size)) { lua_settop(L, top); return NULLOBJECT; }
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
    lua_pushstring(en.st->L, name);
    if (!protect(en.st, p_getglobal, 1)) return NULLOBJECT;
    RexxObjectPtr r = toRexx(en.st, -1);
    lua_pop(en.st->L, 1);
    return r;
}

// ~require(name)
RexxMethod2(RexxObjectPtr, LuaState_Require, CSTRING, name, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    lua_pushstring(en.st->L, "require");
    if (!protect(en.st, p_getglobal, 1)) return NULLOBJECT;
    lua_pushstring(en.st->L, name);
    return firstResult(en.st, pcallAll(en.st, 1));
}

// ~box(type, value): a Lua value of the given type, as a .LuaObject, when
// the rule of *Values* does not give the one needed (docs/design.md, *Boxing*)
RexxMethod3(RexxObjectPtr, LuaState_Box, CSTRING, type, RexxObjectPtr, value, CSELF, self)
{
    Entry en(context, (State *)self, context->GetSelf());
    if (!en.ok) return NULLOBJECT;
    State *st = en.st; lua_State *L = st->L;
    RexxThreadContext *c = context->threadContext;
    std::string t(type);
    for (auto &ch : t) ch = (char)tolower((unsigned char)ch);
    std::string bad;
    if (t == "boolean") {
        logical_t b;
        if (c->ObjectToLogical(value, &b)) lua_pushboolean(L, b ? 1 : 0);
        else bad = "a boolean must be 1 or 0";
    }
    else if (t == "integer") {
        int64_t i;
        if (c->ObjectToInt64(value, &i)) lua_pushinteger(L, (lua_Integer)i);
        else bad = "an integer must be a whole number that fits in 64 bits";
    }
    else if (t == "float") {
        double d;
        if (c->ObjectToDouble(value, &d)) lua_pushnumber(L, (lua_Number)d);
        else bad = "a float must be a number";
    }
    else if (t == "string") {
        RexxStringObject s;
        if (stringValue(c, value, s)) lua_pushlstring(L, c->StringData(s), c->StringLength(s));
        else bad = "the value has no string value";
    }
    else if (t == "reference") {
        if (proxyOf(c, value) != NULL || c->IsString(value) || c->IsOfType(value, "STRING"))
            bad = "only a Rexx object that is not a string goes by reference";
        else pushBox(st, value);
    }
    else bad = "the type must be boolean, integer, float, string or reference, not \"" + std::string(type) + "\"";
    if (c->CheckCondition()) {          // the value's own string method failed: that error
        if (bad.empty()) lua_pop(L, 1);
        return NULLOBJECT;
    }
    if (!bad.empty()) {
        RexxStringObject s;
        if (bad.compare(0, 8, "the type") != 0 && stringValue(c, value, s))
            bad += ", not \"" + std::string(c->StringData(s), c->StringLength(s)) + "\"";
        if (c->CheckCondition()) return NULLOBJECT;
        raise(c, "Lua error: box: " + bad);
        return NULLOBJECT;
    }
    RexxObjectPtr r = newProxy(st, -1);
    lua_pop(L, 1);
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
            if (!pushValue(st, c->SupplierIndex((RexxSupplierObject)sup)) ||
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

/*
 * o~name(args...): the field (any case, see p_resolve); if it is callable, it
 * is called, as o:name(...) or o.name(...) by the colon rule (docs/design.md,
 * *Calls*): a Lua function whose first parameter is `self` gets o; a C
 * function (no parameter names) gets it when it was found through the
 * metatable, not in o itself.  colon: -1 by that rule, 0 or 1 forced
 * (~func, ~method).  all: every result, as an Array (~all).
 * o~name = v: sets the field (p_assign).
 */
static RexxObjectPtr callField(RexxMethodContext *context, Proxy *px, CSTRING message,
                               RexxArrayObject args, int colon, bool all)
{
    State *st = px->st; lua_State *L = st->L;
    RexxThreadContext *c = context->threadContext;
    size_t size = args ? c->ArraySize(args) : 0;
    size_t len = strlen(message);
    int top = lua_gettop(L);
    pushSelf(px);
    int obj = lua_gettop(L);
    if (len > 1 && message[len - 1] == '=' && colon == -1 && !all) {   // assignment
        lua_pushvalue(L, obj);
        lua_pushlstring(L, message, len - 1);
        if (!pushValue(st, size ? c->ArrayAt(args, 1) : NULLOBJECT)) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
        protect(st, p_assign, 3);
        lua_settop(L, top);
        return NULLOBJECT;
    }
    lua_pushvalue(L, obj);
    lua_pushstring(L, message);
    if (!protect(st, p_resolve, 2)) { lua_settop(L, top); return NULLOBJECT; }
    bool raw = lua_toboolean(L, -1);
    lua_pop(L, 1);
    int fn = lua_gettop(L);
    bool callable = lua_type(L, fn) == LUA_TFUNCTION;
    if (!callable && luaL_getmetafield(L, fn, "__call") != LUA_TNIL) { lua_pop(L, 1); callable = true; }
    if (!callable) {
        if (size > 0 || all) {
            lua_settop(L, top);
            raise(c, std::string("Lua error: field '") + message + "' is not callable");
            return NULLOBJECT;
        }
        RexxObjectPtr r = toRexx(st, fn);
        lua_settop(L, top);
        return r;
    }
    if (colon == -1) {
        if (lua_type(L, fn) == LUA_TFUNCTION && !lua_iscfunction(L, fn)) {
            lua_pushvalue(L, fn);
            const char *first = lua_getlocal(L, NULL, 1);   // its first parameter's name
            lua_pop(L, 1);
            colon = first != NULL && strcmp(first, "self") == 0;
        }
        else colon = !raw;
    }
    if (!room(st, (int)size + 1)) { lua_settop(L, top); return NULLOBJECT; }
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

// o~all(name, args...): as o~name(args...), but every result, as an Array
RexxMethod3(RexxObjectPtr, LuaObj_All, CSTRING, name, ARGLIST, args, CSELF, self)
{
    PROXY_ENTRY
    return callField(context, px, name, rest(context, args), -1, true);
}

// o~call(args...): o(...), first result. o~callAll(args...): all results, as an Array
static RexxObjectPtr callSelf(RexxMethodContext *context, Proxy *px, RexxArrayObject args, bool all)
{
    State *st = px->st; lua_State *L = st->L;
    RexxThreadContext *c = context->threadContext;
    int top = lua_gettop(L);
    pushSelf(px);
    size_t size = args ? c->ArraySize(args) : 0;
    if (!room(st, (int)size)) { lua_settop(L, top); return NULLOBJECT; }
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
    lua_pushstring(L, name);
    if (!protect(st, p_resolve, 2)) return NULLOBJECT;
    RexxObjectPtr r = toRexx(st, -2);
    lua_pop(L, 2);
    return r;
}

// o[k], o[k]=v: Lua indexing (keys follow the same rule as values)
RexxMethod2(RexxObjectPtr, LuaObj_At, RexxObjectPtr, key, CSELF, self)
{
    PROXY_ENTRY
    int top = lua_gettop(L);
    pushSelf(px);
    if (!pushValue(st, key)) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
    if (!protect(st, p_gettable, 2)) { lua_settop(L, top); return NULLOBJECT; }
    RexxObjectPtr r = toRexx(st, -1);
    lua_settop(L, top);
    return r;
}

RexxMethod3(RexxObjectPtr, LuaObj_Put, RexxObjectPtr, value, RexxObjectPtr, key, CSELF, self)
{
    PROXY_ENTRY
    int top = lua_gettop(L);
    pushSelf(px);
    if (!pushValue(st, key) || !pushValue(st, value)) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
    protect(st, p_settable, 3);
    lua_settop(L, top);
    return NULLOBJECT;
}

RexxMethod1(RexxObjectPtr, LuaObj_String, CSELF, self)
{
    PROXY_ENTRY
    pushSelf(px);
    if (!protect(st, p_tostring, 1)) return NULLOBJECT;
    size_t len;
    const char *s = lua_tolstring(L, -1, &len);
    RexxObjectPtr r = c->NewString(s, len);
    lua_pop(L, 1);
    return r;
}

// do x over o: the sequence part (1..#o)
RexxMethod1(RexxObjectPtr, LuaObj_MakeArray, CSELF, self)
{
    PROXY_ENTRY
    int top = lua_gettop(L);
    pushSelf(px);
    int obj = lua_gettop(L);
    lua_pushvalue(L, obj);
    if (!protect(st, p_len, 1)) { lua_settop(L, top); return NULLOBJECT; }
    lua_Integer n = lua_tointeger(L, -1);
    lua_pop(L, 1);
    RexxArrayObject a = c->NewArray(n > 0 ? (size_t)n : 0);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_pushvalue(L, obj);
        lua_pushinteger(L, i);
        if (!protect(st, p_gettable, 2)) { lua_settop(L, top); return NULLOBJECT; }
        c->ArrayPut(a, toRexx(st, -1), (size_t)i);
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return a;
}

/*
 * Rexx operators on a .LuaObject: Lua's (docs/design.md, *Operators*), with
 * its metamethods, as ooRexx/Python's are Python's.  The left operand must be
 * the Lua object (Rexx sends the message to it).  "=" and "\=" are Lua's
 * == and ~= (__eq); "==" and "\==" are identity (rawequal), as Rexx
 * collections expect, and ~hashCode agrees with them.  Concatenation is not
 * here: Rexx's own uses the string value (tostring).
 */
enum OpKind { OP_ARITH, OP_COMPARE, OP_RAW };
struct OpEntry { const char *rexx; OpKind kind; int binary; int unary; bool swap; bool negate; };
static const OpEntry operators[] = {
    {"+",   OP_ARITH,   LUA_OPADD,  -2,         false, false},    // -2: unary + is the value itself
    {"-",   OP_ARITH,   LUA_OPSUB,  LUA_OPUNM,  false, false},
    {"*",   OP_ARITH,   LUA_OPMUL,  -1,         false, false},
    {"/",   OP_ARITH,   LUA_OPDIV,  -1,         false, false},
    {"%",   OP_ARITH,   LUA_OPIDIV, -1,         false, false},    // Lua's //: floor division
    {"//",  OP_ARITH,   LUA_OPMOD,  -1,         false, false},    // Lua's %: floor modulo
    {"**",  OP_ARITH,   LUA_OPPOW,  -1,         false, false},
    {"&",   OP_ARITH,   LUA_OPBAND, -1,         false, false},
    {"|",   OP_ARITH,   LUA_OPBOR,  -1,         false, false},
    {"&&",  OP_ARITH,   LUA_OPBXOR, -1,         false, false},
    {"\\",  OP_ARITH,   -1,         LUA_OPBNOT, false, false},
    {"<",   OP_COMPARE, LUA_OPLT,   -1,         false, false},
    {">",   OP_COMPARE, LUA_OPLT,   -1,         true,  false},    // a > b is b < a
    {"<=",  OP_COMPARE, LUA_OPLE,   -1,         false, false},
    {">=",  OP_COMPARE, LUA_OPLE,   -1,         true,  false},
    {"=",   OP_COMPARE, LUA_OPEQ,   -1,         false, false},
    {"\\=", OP_COMPARE, LUA_OPEQ,   -1,         false, true},
    {"<>",  OP_COMPARE, LUA_OPEQ,   -1,         false, true},
    {"><",  OP_COMPARE, LUA_OPEQ,   -1,         false, true},
    {"==",  OP_RAW,     0,          -1,         false, false},
    {"\\==", OP_RAW,    0,          -1,         false, true},
};

RexxMethod3(RexxObjectPtr, LuaObj_Operator, NAME, message, ARGLIST, args, CSELF, self)
{
    PROXY_ENTRY
    const OpEntry *e = NULL;
    for (const auto &o : operators) if (strcmp(o.rexx, message) == 0) { e = &o; break; }
    size_t size = c->ArraySize(args);
    bool unary = size == 0 || c->ArrayAt(args, 1) == NULLOBJECT;
    int op = e == NULL ? -1 : unary ? e->unary : e->binary;
    if (op == -1) {
        raise(c, std::string("Lua error: operator ") + message + " needs " + (unary ? "two operands" : "one operand"));
        return NULLOBJECT;
    }
    if (op == -2) return context->GetSelf();                        // unary +
    int top = lua_gettop(L);
    if (e->kind == OP_RAW) {
        pushSelf(px);
        if (!pushValue(st, c->ArrayAt(args, 1))) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
        bool eq = lua_rawequal(L, -1, -2);
        lua_settop(L, top);
        return (eq != e->negate) ? c->True() : c->False();
    }
    lua_pushinteger(L, op);
    if (e->swap && !pushValue(st, c->ArrayAt(args, 1))) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
    pushSelf(px);
    if (!unary && !e->swap && !pushValue(st, c->ArrayAt(args, 1))) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
    if (!protect(st, e->kind == OP_ARITH ? p_arith : p_compare, unary ? 2 : 3)) { lua_settop(L, top); return NULLOBJECT; }
    RexxObjectPtr r;
    if (e->kind == OP_COMPARE) r = (lua_toboolean(L, -1) != e->negate) ? c->True() : c->False();
    else r = toRexx(st, -1);
    lua_settop(L, top);
    return r;
}

// ~hashCode: consistent with "==" (rawequal), for Rexx collections: the
// address of a table, function, userdata or thread; the value of the rest
RexxMethod1(RexxObjectPtr, LuaObj_HashCode, CSELF, self)
{
    PROXY_ENTRY
    pushSelf(px);
    char buf[64];
    switch (lua_type(L, -1)) {
    case LUA_TTABLE: case LUA_TFUNCTION: case LUA_TUSERDATA: case LUA_TTHREAD: case LUA_TLIGHTUSERDATA:
        snprintf(buf, sizeof buf, "%p", lua_topointer(L, -1));
        break;
    case LUA_TBOOLEAN:
        snprintf(buf, sizeof buf, "%s", lua_toboolean(L, -1) ? "true" : "false");
        break;
    case LUA_TNUMBER: {             // 1 and 1.0 are rawequal: the same hash
        lua_Integer i;
        int isInt = lua_isinteger(L, -1);
        if (!isInt) {
            double d = (double)lua_tonumber(L, -1);
            isInt = d == std::floor(d) && d >= -9.2233720368547758e18 && d < 9.2233720368547758e18;
            i = isInt ? (lua_Integer)d : 0;
        }
        else i = lua_tointeger(L, -1);
        if (isInt) snprintf(buf, sizeof buf, "%lld", (long long)i);
        else snprintf(buf, sizeof buf, "%a", (double)lua_tonumber(L, -1));
        break;
    }
    case LUA_TSTRING: {
        size_t n;
        const char *str = lua_tolstring(L, -1, &n);
        RexxObjectPtr r = c->NewString(str, n);
        lua_pop(L, 1);
        return r;
    }
    default:
        snprintf(buf, sizeof buf, "nil");
    }
    lua_pop(L, 1);
    return c->String(buf);
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
    REXX_METHOD(LuaState_Box,       LuaState_Box),
    REXX_METHOD(LuaState_Table,     LuaState_Table),
    REXX_METHOD(LuaState_Predicate, LuaState_Predicate),
    REXX_METHOD(LuaState_Stats,     LuaState_Stats),
    REXX_METHOD(LuaObj_Unknown,     LuaObj_Unknown),
    REXX_METHOD(LuaObj_Method,      LuaObj_Method),
    REXX_METHOD(LuaObj_Func,        LuaObj_Func),
    REXX_METHOD(LuaObj_All,         LuaObj_All),
    REXX_METHOD(LuaObj_Call,        LuaObj_Call),
    REXX_METHOD(LuaObj_CallAll,     LuaObj_CallAll),
    REXX_METHOD(LuaObj_Get,         LuaObj_Get),
    REXX_METHOD(LuaObj_At,          LuaObj_At),
    REXX_METHOD(LuaObj_Put,         LuaObj_Put),
    REXX_METHOD(LuaObj_String,      LuaObj_String),
    REXX_METHOD(LuaObj_MakeArray,   LuaObj_MakeArray),
    REXX_METHOD(LuaObj_Type,        LuaObj_Type),
    REXX_METHOD(LuaObj_Operator,    LuaObj_Operator),
    REXX_METHOD(LuaObj_HashCode,    LuaObj_HashCode),
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
