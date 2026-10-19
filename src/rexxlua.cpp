/*
 * rexxlua.cpp -- librexxlua (.so, .dylib; rexxlua.dll): Lua embedded in
 * ooRexx, as a native package.
 * Rexx side: lua.cls (.lua, .LuaState, .LuaObject).  How it works and why:
 * docs/design.md (the comments name its sections, e.g. *States*).
 *
 * Values (docs/design.md, *Values*), Lua -> Rexx: nil -> .nil; boolean ->
 *   .Boolean~true/.Boolean~false; integer -> its decimal string; float -> the shortest string that
 *   gives it back; string -> string; a Rexx object passed in earlier -> that
 *   object; anything else -> a .LuaObject proxy.
 * Rexx -> Lua: .nil -> nil; a .Boolean -> boolean; .LuaObject -> its value;
 *   Array -> a new sequence table (copy); a string -> integer, float or string, by a rule that
 *   loses nothing (numberKind, pushRexxString); Routine -> a Lua function;
 *   any other object -> userdata "RexxObject" that Lua can call or send
 *   messages to.
 */

#include <oorexxapi.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#if LUA_VERSION_NUM < 503
#error "the bridge needs Lua 5.3 or later (integers)"
#endif
#if LUA_VERSION_NUM < 504                   // Lua 5.3: one user value, always
#define lua_newuserdatauv(L, size, nuvalue) lua_newuserdata(L, size)
#endif
#ifndef _WIN32
#include <dlfcn.h>
#include <signal.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <xlocale.h>                        // strtod_l
#endif
#include <clocale>
#include <atomic>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cerrno>
#include <cstdlib>

// luaopen_oorexx, the entry point of the Lua module (*Lua → ooRexx*)
#ifdef _WIN32
#define MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif
MODULE_EXPORT int luaopen_oorexx(lua_State *L);

/* A function kept out of its callers.  The helpers that make C++ objects
 * (std::string, std::vector) are, so that those objects never live in the
 * frame of a Lua C function that raises a Lua error: MSVC's longjmp unwinds
 * the stack as an exception does, and with /EHsc, which assumes that C
 * functions such as lua_error do not throw, it destroys again the objects of
 * a scope already left (docs/design.md, *Errors*).                        */
#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

/* strcasecmp, which MSVC does not have: 0 if equal, ignoring ASCII case */
static int strcmpNoCase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 1;
    return *a != *b;
}

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
    // A host's lua_State (Lua -> ooRexx: docs/design.md, *Lua → ooRexx*):
    bool adopted = false;
    std::thread::id owner;                  // the host's thread, the only one that may use it
    struct Inst *inst = NULL;               // the instance that calls from Lua into Rexx
                                            // attach to (outside any Rexx code)
    struct Inst *defaultInst = NULL;        // the module's default instance (made at first use)
    std::vector<RexxObjectPtr> pending;     // Rexx objects whose userdata Lua finalized outside
                                            // any Rexx code: let go at the next entry
    struct Redirect *redirect = NULL;       // ADDRESS LUA ... WITH in progress, innermost first
    lua_Integer redirects = 0;              // the last one's number
    struct Command *command = NULL;         // commands of Lua handlers in progress, innermost first
    lua_Integer commands = 0;               // the last one's number
    // A C++ exception that ooRexx threw while Lua code called Rexx (a SIGNAL
    // unwinding to a Rexx caller above the bridge, docs/design.md, *Errors*),
    // caught before it crossed Lua's frames: Lua unwinds by a Lua error, and
    // the bridge's lua_pcall throws it again (rethrowUnwind)
    std::exception_ptr unwind;
    // Ctrl-C (docs/design.md, *Ctrl-C*); read by the signal handler
    std::atomic<int> running{0};            // lua_pcalls of the bridge's in progress (nested too)
    std::atomic<bool> interrupted{false};   // a signal stopped its Lua code: until the outer
                                            // lua_pcall returns, the stop hook stops it
    State *nextAll = NULL;                  // every State, for the handler (allStates)
    // the coroutines being resumed, outermost first (auxresume): the handler
    // stops them too.  The first MAXCO - 1, and the innermost always in the
    // last slot (the one running); coDepth counts all
    std::atomic<lua_State *> coStack[16] = {};
    std::atomic<int> coDepth{0};
};
static const int MAXCO = 16;

// One per .LuaObject: a Lua value in a state's registry.
struct Proxy {
    int kind = KIND_PROXY;
    State *st;
    int ref;
};

// registry[&STATEKEY]: the State of a lua_State (a light userdata).  Not Lua's
// extra space, which belongs to the host when the state is adopted.
// (Not const: MSVC may fold equal constants into one address, and the keys
// must differ.)
static char STATEKEY;
static State *stateOf(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &STATEKEY);
    State *st = (State *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return st;
}

static bool foreignThread(State *st) { return st->adopted && std::this_thread::get_id() != st->owner; }

// Entering a state: its lock, and who is inside (restored on exit, so that
// entries nest: Rexx -> Lua -> Rexx -> Lua on one thread).
// mode: LOCK waits for the lock, TRY does not, SKIP does not enter at all.
enum EnterMode { LOCK, TRY, SKIP };
// Left by a C++ exception (an unwind that rethrowUnwind threw again), it also
// puts back the Lua thread in use and its stack top, which the code it
// skipped would have.
struct Enter {
    State *st;
    std::unique_lock<std::recursive_mutex> lk;
    RexxThreadContext *savedTc;
    RexxObjectPtr savedSelf;
    lua_State *savedL;
    int savedTop = 0;
    int exceptions;
    bool owns;
    Enter(State *s, RexxThreadContext *tc, RexxObjectPtr self, EnterMode mode = LOCK)
        : st(s), lk(s->lock, std::defer_lock), exceptions(std::uncaught_exceptions())
    {
        owns = mode == TRY ? lk.try_lock() : mode == LOCK ? (lk.lock(), true) : false;
        savedTc = st->tc; savedSelf = st->self; savedL = st->L;
        if (owns) {
            st->tc = tc; if (self != NULLOBJECT) st->self = self; st->depth++;
            if (savedL != NULL && !st->closed) savedTop = lua_gettop(savedL);
        }
    }
    ~Enter()
    {
        if (!owns) return;
        if (std::uncaught_exceptions() > exceptions && !st->closed && savedL != NULL) {
            st->L = savedL;
            if (lua_gettop(savedL) > savedTop) lua_settop(savedL, savedTop);
        }
        st->depth--; st->tc = savedTc; st->self = savedSelf;
    }
};

/* Rexx code that Lua calls (a Rexx method, a supplier, a string method...),
 * from a Lua C function: f makes the API calls.  ooRexx unwinds a SIGNAL by
 * a C++ throw, and when the trap is in a Rexx caller above the bridge
 * (Rexx -> .lua~eval -> Lua -> a Rexx method that does RAISE USER X ...
 * RETURN, the caller with SIGNAL ON USER X) the throw would cross Lua's C
 * frames and lua_pcall, which it cannot unwind (their counters stay: after
 * ~200 such, "C stack overflow"; on Windows it is undefined).  So it stops
 * here, in st->unwind, f is not run while one is pending, and the caller
 * raises a Lua error (unwindError); the bridge's lua_pcall throws it again
 * once Lua has unwound (rethrowUnwind).  False if f did not complete.      */
template <class F> static NOINLINE bool guarded(State *st, F &&f)
{
    if (st->unwind) return false;
    try { f(); return true; }
    catch (...) { st->unwind = std::current_exception(); return false; }
}

static int unwindError(lua_State *L)
{
    lua_pushliteral(L, "a Rexx SIGNAL is unwinding through Lua");
    return lua_error(L);
}

// After the bridge's lua_pcall (or what it did after it): an unwind that
// stopped in Lua (guarded) goes on, as the C++ exception it was
static void rethrowUnwind(State *st)
{
    if (!st->unwind) return;
    std::exception_ptr e = st->unwind;
    st->unwind = nullptr;
    std::rethrow_exception(e);
}

static void raise(RexxThreadContext *c, const std::string &m)
{
    c->RaiseException1(Rexx_Error_Execution_user_defined, c->NewString(m.data(), m.size()));   // a NUL stays
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
// .LuaObject (the latest one activated): by name it is found only where
// lua.cls is visible, and Lua -> ooRexx calls run in no package
static RexxClassObject luaObjectClass = NULLOBJECT;

static void closeState(RexxThreadContext *c, State *st);
static void release(RexxThreadContext *c, RexxObjectPtr o);

static NOINLINE void drain(RexxThreadContext *c)
{
    if (graveyard == NULLOBJECT) return;
    std::vector<void *> later;
    for (;;) {
        RexxObjectPtr p = c->SendMessage0(graveyard, "PULL");
        if (p == NULLOBJECT || p == c->Nil()) break;
        void *v = c->PointerValue((RexxPointerObject)p);
        release(c, p);
        if (v == NULL) continue;
        if (*(int *)v == KIND_PROXY) {
            Proxy *px = (Proxy *)v;
            if (px->st->closed) { delete px; continue; }
            if (foreignThread(px->st)) { later.push_back(v); continue; }   // the host's thread will
            Enter e(px->st, c, NULLOBJECT, TRY);
            if (!e.owns) { later.push_back(v); continue; }
            if (!px->st->closed) luaL_unref(px->st->L, LUA_REGISTRYINDEX, px->ref);
            delete px;
        }
        else {
            State *st = (State *)v;
            if (st->adopted) continue;          // closed by its host only
            Enter e(st, c, NULLOBJECT, TRY);
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

// What a Rexx string may become in Lua (docs/design.md, *Values*): the same
// test as ooRexx/Python's (its bridge.cpp, numberKind); pushRexxString then
// keeps a float only if it loses nothing.
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

static std::string formatFloat(double d);

// strtod in the "C" locale, whatever LC_NUMERIC the process has: a Lua
// program may call os.setlocale, and then strtod would stop at the point of
// "0.25" (oorexx-bridges, inbox/lua.md, 19/10/2026)
static double cStrtod(const char *s)
{
#ifdef _WIN32
    static _locale_t c = _create_locale(LC_NUMERIC, "C");
    return _strtod_l(s, NULL, c);
#else
    static locale_t c = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    return strtod_l(s, NULL, c);
#endif
}

// Push a Rexx string by that rule, which loses nothing: an integer that does
// not fit in 64 bits stays a string (a float would lose digits silently), and
// so does a decimal number that the float would not write back the same
// ("3.10", "1E3", "1.26765060E+30"; "0.5" and "100.0" are floats), so that
// Rexx's formatting reaches Lua (docs/design.md, *Values*).
static NOINLINE void pushRexxString(lua_State *L, const char *d, size_t n)
{
    std::string s(d, n);                    // NUL-terminated, for strtoll/strtod
    switch (numberKind(d, n)) {
    case INTEGER_NUMBER: {
        errno = 0;
        long long v = strtoll(s.c_str(), NULL, 10);
        if (errno == 0) { lua_pushinteger(L, (lua_Integer)v); return; }
        break;
    }
    case FLOAT_NUMBER: {
        double v = cStrtod(s.c_str());
        if (formatFloat(v) == s) { lua_pushnumber(L, (lua_Number)v); return; }
        break;
    }
    default:
        break;
    }
    lua_pushlstring(L, d, n);
}

// A Lua float as Rexx sees it (the family's rule, oorexx-bridges
// topics/float-text.md, decided 18/10/2026): the shortest digits that give
// the same double back, written as ooRexx writes x + 0 under NUMERIC DIGITS
// 17: plain unless the integer part needs more than 17 places or the
// fraction more than 34, else one digit, a point, the rest, "E", the sign
// and the exponent (1E+17, 1.5E-35); ".0" when the value is an integer
// written without an exponent, because Lua tells integers from floats by the
// text (1.0, -0.0, 9007199254740992.0); nan, +infinity, -infinity, as ooRexx
// writes them (DoubleToObject) and reads them back (ObjectToDouble).  Lua's
// own %.14g would lose digits: 0.1 + 0.2 would be "0.3".
static NOINLINE std::string formatFloat(double d)
{
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "+infinity" : "-infinity";
    std::string sign = std::signbit(d) ? "-" : "";
    if (d == 0) return sign + "0.0";
    // The shortest digits: %.*e with 1 to 17 significant digits.  It writes
    // "D<point>DDDe+XX" with the locale's decimal point (a comma, or several
    // bytes): only the ASCII digits are taken, and the text that is read back
    // is rebuilt with a "." for cStrtod
    char buf[64];
    std::string digits;
    int x = 0;                              // d = D.DDD x 10^x
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, fabs(d));
        char *e = strchr(buf, 'e');
        x = atoi(e + 1);
        digits.clear();
        for (char *p = buf; p < e; p++) if (*p >= '0' && *p <= '9') digits += *p;
        std::string c = digits.substr(0, 1) + "." + digits.substr(1) + "e" + std::to_string(x);
        if (cStrtod(c.c_str()) == fabs(d)) break;
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int n = (int)digits.size();
    int places = n - 1 - x;                 // after the point, leading zeros included
    if (x + 1 > 17 || places > 34) {
        std::string r = sign + digits.substr(0, 1);
        if (n > 1) r += "." + digits.substr(1);
        return r + "E" + (x < 0 ? "-" : "+") + std::to_string(x < 0 ? -x : x);
    }
    if (x < 0) return sign + "0." + std::string(-x - 1, '0') + digits;
    if (n <= x + 1) return sign + digits + std::string(x + 1 - n, '0') + ".0";
    return sign + digits.substr(0, x + 1) + "." + digits.substr(x + 1);
}

static RexxObjectPtr toRexx(State *st, int idx);
static bool pushValue(State *st, RexxObjectPtr v, int memo = 0, int depth = 0);

static bool takeInterrupt(State *st);
static void coClear(State *st);
static RexxObjectPtr failed(State *st);

/*
 * Rexx errors in Lua (docs/design.md, *Errors*).  A Rexx condition becomes a
 * table with its fields (pushRexxCondition) and the metatable RXERR, whose
 * __tostring gives the text the error was until 0.3.0.  The weak-keyed table
 * registry[&RXCOND] maps each such table to the userdata of its condition
 * object, so that the bridge raises that condition again when the table comes
 * back to Rexx (raiseCondition), instead of a Lua error around it.
 */
static const char *RXERR = "RexxError";
static char RXCOND;

static void rawField(lua_State *L, int t, const char *name)    // pushes t[name], no metamethods
{
    lua_pushstring(L, name);
    lua_rawget(L, t);
}

// tostring(e): "Rexx error 41.1: message" (the secondary message, else the
// error text); a condition other than SYNTAX, "Rexx condition NAME"
static int rxerr_tostring(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    rawField(L, 1, "code");                                 // 2
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_pushliteral(L, "Rexx error ");
        lua_pushvalue(L, 2);
        rawField(L, 1, "message");
        if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); rawField(L, 1, "errortext"); }
        if (lua_type(L, -1) == LUA_TSTRING) { lua_pushliteral(L, ": "); lua_insert(L, -2); lua_concat(L, 4); }
        else { lua_pop(L, 1); lua_concat(L, 2); }          // (lua_concat: a NUL stays)
        return 1;
    }
    lua_pushliteral(L, "Rexx condition ");
    rawField(L, 1, "condition");
    if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); lua_pushliteral(L, "?"); }
    lua_concat(L, 2);
    return 1;
}

// Is the value at idx a Rexx error (its metatable is RXERR)?
static bool isRxErr(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TTABLE || !lua_getmetatable(L, idx)) return false;
    luaL_getmetatable(L, RXERR);
    bool ours = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return ours;
}

// a .. b, with a Rexx error on either side: its text (the other side, a
// string, a number or a Rexx error too, as Lua's own .. wants)
static int rxerr_concat(lua_State *L)
{
    for (int i = 1; i <= 2; i++) {
        int t = lua_type(L, i);
        if (t != LUA_TSTRING && t != LUA_TNUMBER && !isRxErr(L, i))
            return luaL_error(L, "attempt to concatenate a %s value", luaL_typename(L, i));
    }
    luaL_tolstring(L, 1, NULL);
    luaL_tolstring(L, 2, NULL);
    lua_concat(L, 2);
    return 1;
}

// Pushes the metatable of Rexx errors (made at first use, in each state)
static void pushRxErrMeta(lua_State *L)
{
    if (luaL_newmetatable(L, RXERR)) {                      // also sets __name
        lua_pushstring(L, RXERR);           // getmetatable(e): "RexxError", and the
        lua_setfield(L, -2, "__metatable"); // metatable, shared, cannot be changed
        lua_pushcfunction(L, rxerr_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, rxerr_concat);
        lua_setfield(L, -2, "__concat");
    }
}

// The message handler of the bridge's lua_pcalls.  The error value, its text
// (luaL_tolstring, which runs __tostring: here it is protected) and Lua's
// traceback, as {value, text, traceback, &FAILMARK}
static char FAILMARK;
static int msgh(lua_State *L)
{
    lua_settop(L, 1);
    lua_createtable(L, 4, 0);                               // 2
    lua_pushvalue(L, 1);
    lua_rawseti(L, 2, 1);
    luaL_tolstring(L, 1, NULL);
    lua_rawseti(L, 2, 2);
    luaL_traceback(L, L, NULL, 1);
    lua_rawseti(L, 2, 3);
    lua_pushlightuserdata(L, &FAILMARK);
    lua_rawseti(L, 2, 4);
    return 1;
}

// A Lua error: its text (the message) and Lua's traceback
struct LuaFailure {
    std::string text, traceback;
    std::string full() const { return traceback.empty() ? text : text + "\n" + traceback; }
};

static void setString(std::string &s, lua_State *L, int idx)
{
    size_t length;
    const char *m = lua_tolstring(L, idx, &length);
    if (m != NULL) s.assign(m, length); else s.clear();     // a NUL stays
}

/* Run the function and nargs arguments on top of the stack, protected.
 * Leaves all results on the stack and returns their count, or -1 with the
 * error value on top of the stack (the caller pops it), and its text and
 * Lua's traceback in f (nothing raised).                                  */
static int pcallQuiet(State *st, int nargs, LuaFailure &f)
{
    lua_State *L = st->L;
    int base = lua_gettop(L) - nargs;       // index of the function
    lua_pushcfunction(L, msgh);
    lua_insert(L, base);
    if (st->running.load() == 0) st->interrupted.store(false);   // a signal that came as the last
                                                                 // call ended: ooRexx's HALT
    st->running++;
    int rc = lua_pcall(L, nargs, LUA_MULTRET, base);
    if (--st->running == 0) coClear(st);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        int e = lua_gettop(L);
        bool wrapped = false;               // by msgh (not when out of memory, or msgh failed)
        if (lua_type(L, e) == LUA_TTABLE) {
            lua_rawgeti(L, e, 4);
            wrapped = lua_touserdata(L, -1) == &FAILMARK;
            lua_pop(L, 1);
        }
        if (wrapped) {
            lua_rawgeti(L, e, 2); setString(f.text, L, -1); lua_pop(L, 1);
            lua_rawgeti(L, e, 3); setString(f.traceback, L, -1); lua_pop(L, 1);
            lua_rawgeti(L, e, 1);
            lua_replace(L, e);
        }
        else {
            if (lua_type(L, e) == LUA_TSTRING) setString(f.text, L, e); else f.text = "(error object)";
            f.traceback.clear();
        }
        return -1;
    }
    return lua_gettop(L) - base + 1;
}

static RexxObjectPtr conditionOf(lua_State *L, int idx);

// A HALT that ended a callback (Error 4)
static bool isHalt(RexxThreadContext *c, RexxObjectPtr cond)
{
    RexxCondition cd;
    c->DecodeConditionInfo((RexxDirectoryObject)cond, &cd);
    return cd.code / 1000 == 4;
}

// Raise again a Rexx condition that went through Lua: SYNTAX with its code
// and substitutions (a new traceback, from here), any other with its
// description, additional and result
static void raiseCondition(RexxThreadContext *c, RexxObjectPtr cond)
{
    RexxCondition cd;
    RexxDirectoryObject d = (RexxDirectoryObject)cond;
    c->DecodeConditionInfo(d, &cd);
    if (cd.code != 0) {
        c->RaiseException(cd.code, cd.additional != NULLOBJECT ? cd.additional : c->NewArray(0));
        return;
    }
    c->RaiseCondition(c->CString(cd.conditionName), cd.description,
                      c->DirectoryAt(d, "ADDITIONAL"), c->DirectoryAt(d, "RESULT"));
}

static RexxObjectPtr newProxy(State *st, int idx);

/* A Lua error (the value at idx) as a Rexx condition: SYNTAX 98.900 with
 * additional [1] "Lua error: " text, [2] the value (a string, or the Rexx
 * object a userdata holds, or a .LuaObject; .nil is left out), [3] Lua's
 * traceback.                                                              */
static NOINLINE void raiseLuaError(State *st, int idx, const LuaFailure &f)
{
    RexxThreadContext *c = st->tc;
    idx = lua_absindex(st->L, idx);
    std::string m = "Lua error: " + f.text;
    RexxArrayObject a = c->NewArray(3);
    c->ArrayPut(a, c->NewString(m.data(), m.size()), 1);
    int t = lua_type(st->L, idx);
    RexxObjectPtr v = t == LUA_TNUMBER || t == LUA_TBOOLEAN ? newProxy(st, idx) : toRexx(st, idx);
    if (v != NULLOBJECT && v != c->Nil()) c->ArrayPut(a, v, 2);
    if (!f.traceback.empty()) c->ArrayPut(a, c->NewString(f.traceback.data(), f.traceback.size()), 3);
    if (c->CheckCondition()) return;                        // making the .LuaObject failed: that error
    c->RaiseException(Rexx_Error_Execution_user_defined, a);
}

/* The same, raising a Rexx condition.  Leaves all results on the stack and
 * returns their count, or -1 after raising a Rexx condition: the one a Rexx
 * error that went through Lua came from (Error 4.1 for a HALT in a callback),
 * or for any other Lua error SYNTAX 98.900 (raiseLuaError).
 * Ctrl-C (docs/design.md, *Ctrl-C*): the caller's HALT is made pending; if
 * that stopped the Lua code, -1 with no condition (see failed).            */
static int pcallAll(State *st, int nargs)
{
    LuaFailure f;
    int n = pcallQuiet(st, nargs, f);
    if (st->unwind) {                       // a SIGNAL past the bridge (guarded): it goes on
        if (n < 0) lua_pop(st->L, 1); else lua_pop(st->L, n);
        rethrowUnwind(st);
    }
    bool ctrlC = takeInterrupt(st);
    RexxObjectPtr cond = n < 0 ? conditionOf(st->L, -1) : NULLOBJECT;
    if (cond != NULLOBJECT && isHalt(st->tc, cond)) {   // a HALT in a callback: Error 4.1, as ADDRESS LUA
        raiseCondition(st->tc, cond);
        lua_pop(st->L, 1);
        return -1;
    }
    if (ctrlC) st->tc->HaltThread();            // (not twice if ooRexx's handler already did)
    if (n < 0) {
        if (!ctrlC) {
            if (cond != NULLOBJECT) raiseCondition(st->tc, cond);
            else raiseLuaError(st, -1, f);
        }
        lua_pop(st->L, 1);
    }
    return n;
}

/* What a method returns when pcallAll returned -1: NULLOBJECT, with the
 * condition raised; or, after Ctrl-C, .nil, so that the clause ends and the
 * HALT comes at its boundary (a missing result would raise Error 91 first). */
static RexxObjectPtr failed(State *st)
{
    return st->tc->CheckCondition() ? NULLOBJECT : st->tc->Nil();
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
            if (lua_type(L, -2) == LUA_TSTRING && strcmpNoCase(lua_tostring(L, -2), name) == 0) {
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
                if (lua_type(L, -1) == LUA_TSTRING && strcmpNoCase(lua_tostring(L, -1), name) == 0) {
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
// v -> its size as a sequence: a table's integer field n (raw: an Array with
// empty slots carries it, as table.pack does; docs/design.md, *Values*), else #v
static int p_size(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TTABLE) {
        lua_pushstring(L, "n");
        lua_rawget(L, 1);
        if (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 0) return 1;
        lua_settop(L, 1);
    }
    lua_pushinteger(L, luaL_len(L, 1));
    return 1;
}
static int p_getglobal(lua_State *L) { lua_getglobal(L, lua_tostring(L, 1)); return 1; }
// obj -> keys, values, n: what pairs(obj) gives, as two sequences.  pairs
// honours __pairs (Lua 5.3 to 5.5), so this does too; without it, the raw
// table (lua_next), whatever the global pairs is now
static int p_pairs(lua_State *L)
{
    lua_settop(L, 1);
    lua_newtable(L);                                    // 2: keys
    lua_newtable(L);                                    // 3: values
    lua_Integer n = 0;
    if (luaL_getmetafield(L, 1, "__pairs") != LUA_TNIL) {        // 4
        lua_pushvalue(L, 1);
        lua_call(L, 1, 3);                              // 4 f, 5 s, 6 control
        for (;;) {
            lua_pushvalue(L, 4); lua_pushvalue(L, 5); lua_pushvalue(L, 6);
            lua_call(L, 2, 2);                          // 7 k, 8 v
            if (lua_isnil(L, 7)) break;
            n++;
            lua_rawseti(L, 3, n);                       // v
            lua_pushvalue(L, 7);
            lua_rawseti(L, 2, n);
            lua_replace(L, 6);                          // k: the next control
        }
        lua_settop(L, 3);
    }
    else {
        if (lua_type(L, 1) != LUA_TTABLE)
            return luaL_error(L, "only a table, or a value with __pairs, can be enumerated");
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            n++;
            lua_rawseti(L, 3, n);
            lua_pushvalue(L, -1);
            lua_rawseti(L, 2, n);
        }
    }
    lua_pushinteger(L, n);
    return 3;
}
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
    if (n < 0) return failed(st);
    RexxObjectPtr r = n == 0 ? st->tc->Nil() : toRexx(st, -n);
    lua_pop(st->L, n);
    return r;
}

/* All results as a Rexx Array; pops them. */
static RexxObjectPtr allResults(State *st, int n)
{
    if (n < 0) return failed(st);
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
// obj is NULL once its __gc has run.  inst: in a host's state, the instance
// whose Rexx code made the userdata, which calls from Lua attach to.
struct RexxBox { RexxObjectPtr obj; struct Inst *inst; };

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

// With st entered: let go of the Rexx objects whose userdata Lua finalized
// while no Rexx code was running (an adopted state's, see box_gc)
static NOINLINE void releasePending(State *st)
{
    if (st->pending.empty() || st->tc == NULL) return;
    std::vector<RexxObjectPtr> objs;
    objs.swap(st->pending);
    for (RexxObjectPtr o : objs) keepRemove(st, o);
}

static void pushBox(State *st, RexxObjectPtr v);

static void setStringField(lua_State *L, RexxThreadContext *c, const char *name, RexxStringObject s)
{
    if (s == NULLOBJECT) return;
    lua_pushlstring(L, c->StringData(s), c->StringLength(s));  // a NUL stays
    lua_setfield(L, -2, name);
}

/* A Rexx condition, pending on this thread, as a Lua error (pushes it; the
 * caller then returns lua_error(L)): a table with the condition's fields
 * (condition, code "41.1", rc, errortext, message, program, line,
 * traceback), the metatable RXERR, and the condition object kept in
 * registry[&RXCOND] for the way back (docs/design.md, *Errors*).          */
static Proxy *proxyOf(RexxThreadContext *c, RexxObjectPtr v);

/* The way back of a Lua error (raiseLuaError): a SYNTAX 98.900 whose
 * additional is [1] "Lua error: ...", [2] the error value, coming back to
 * Lua, is that value again (a string as it was, a .LuaObject of this state
 * its Lua value, a Rexx object its userdata).  False if it is not one.    */
static bool pushLuaError(lua_State *L, RexxThreadContext *c, const RexxCondition &cd)
{
    if (cd.code != Rexx_Error_Execution_user_defined || cd.additional == NULLOBJECT) return false;
    if (c->ArrayItems(cd.additional) < 2) return false;
    RexxObjectPtr m = c->ArrayAt(cd.additional, 1);
    RexxObjectPtr v = c->ArrayAt(cd.additional, 2);
    bool ours = m != NULLOBJECT && c->IsString(m) && c->StringLength((RexxStringObject)m) >= 11 &&
                memcmp(c->StringData((RexxStringObject)m), "Lua error: ", 11) == 0 &&
                v != NULLOBJECT && v != c->Nil();
    release(c, m);
    if (!ours) { release(c, v); return false; }
    State *st = stateOf(L);
    Proxy *p = proxyOf(c, v);
    bool done = true;
    if (p != NULL) {
        if (st != NULL && p->st == st && !st->closed) lua_rawgeti(L, LUA_REGISTRYINDEX, p->ref);
        else done = false;                  // another state's value
    }
    else if (c->IsString(v)) lua_pushlstring(L, c->StringData((RexxStringObject)v), c->StringLength((RexxStringObject)v));
    else if (st != NULL && st->tc == c && !c->IsArray(v)) {
        lua_State *saved = st->L;
        st->L = L;
        pushBox(st, v);                     // a Rexx object that Lua raised
        st->L = saved;
    }
    else done = false;
    release(c, v);
    return done;
}

static NOINLINE void pushRexxCondition(lua_State *L, RexxThreadContext *c)
{
    RexxDirectoryObject cond = c->GetConditionInfo();
    RexxCondition cd;
    c->DecodeConditionInfo(cond, &cd);
    c->ClearCondition();
    if (pushLuaError(L, c, cd)) { release(c, cond); return; }
    lua_createtable(L, 0, 8);
    setStringField(L, c, "condition", cd.conditionName);
    if (cd.code != 0) {
        lua_pushfstring(L, "%d.%d", (int)(cd.code / 1000), (int)(cd.code % 1000));
        lua_setfield(L, -2, "code");
    }
    lua_pushinteger(L, (lua_Integer)cd.rc);
    lua_setfield(L, -2, "rc");
    setStringField(L, c, "errortext", cd.errortext);
    setStringField(L, c, "message", cd.message);
    setStringField(L, c, "program", cd.program);
    if (cd.position != 0) { lua_pushinteger(L, (lua_Integer)cd.position); lua_setfield(L, -2, "line"); }
    RexxObjectPtr tb = c->DirectoryAt(cond, "TRACEBACK");  // a List of lines
    if (tb != NULLOBJECT && tb != c->Nil()) {
        RexxObjectPtr a = c->SendMessage0(tb, "MAKEARRAY");
        if (a != NULLOBJECT && c->IsArray(a)) {
            luaL_Buffer lines;                  // (no C++ object here: Lua may raise
            luaL_buffinit(L, &lines);           // out of memory)
            size_t n = c->ArrayItems((RexxArrayObject)a), k = 0;
            for (size_t i = 1; i <= n; i++) {
                RexxObjectPtr line = c->ArrayAt((RexxArrayObject)a, i);
                if (line == NULLOBJECT || !c->IsString(line)) continue;
                if (k++ > 0) luaL_addchar(&lines, '\n');
                luaL_addlstring(&lines, c->StringData((RexxStringObject)line), c->StringLength((RexxStringObject)line));
                release(c, line);
            }
            luaL_pushresult(&lines);
            lua_setfield(L, -2, "traceback");
        }
        release(c, a);
        release(c, tb);
    }
    c->ClearCondition();                    // (in case MAKEARRAY raised one)
    pushRxErrMeta(L);
    lua_setmetatable(L, -2);
    State *st = stateOf(L);
    if (st != NULL && st->tc == c && !st->closed) {         // the condition object, for the way back
        lua_State *saved = st->L;
        st->L = L;
        lua_rawgetp(L, LUA_REGISTRYINDEX, &RXCOND);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_createtable(L, 0, 1);
            lua_pushliteral(L, "k");
            lua_setfield(L, -2, "__mode");
            lua_setmetatable(L, -2);
            lua_pushvalue(L, -1);
            lua_rawsetp(L, LUA_REGISTRYINDEX, &RXCOND);
        }
        lua_pushvalue(L, -2);                               // e conds e
        pushBox(st, cond);                                  // e conds e box
        lua_rawset(L, -3);
        lua_pop(L, 1);
        st->L = saved;
    }
    release(c, cond);                       // the userdata keeps it
}

// The Rexx condition that the Lua error value at idx came from (a table of
// pushRexxCondition), or NULLOBJECT
static RexxObjectPtr conditionOf(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    if (!isRxErr(L, idx)) return NULLOBJECT;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &RXCOND);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return NULLOBJECT; }
    lua_pushvalue(L, idx);
    lua_rawget(L, -2);
    RexxBox *b = (RexxBox *)luaL_testudata(L, -1, REXXOBJ);
    RexxObjectPtr cond = b != NULL ? b->obj : NULLOBJECT;
    lua_pop(L, 2);
    return cond;
}

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

/*
 * In a host's state (Lua -> ooRexx), Lua runs outside any Rexx code too, and
 * may then call a Rexx object it holds, or the module's functions.  Such a
 * call (st->tc is NULL) goes through hostRun: it attaches the thread to an
 * interpreter instance, runs the same C function again inside lua_pcall
 * (marked by GUARD as its first argument), and then, whatever happened,
 * puts the State back and detaches before raising the error, if any.  So a
 * Lua error anywhere in between (an allocation's, a 5.3 finalizer's) cannot
 * leave the thread attached or the State changed.  docs/design.md,
 * *Lua → ooRexx*.
 */
static char GUARD;
static const char *noRexx(State *st);
static int fromHost(lua_State *L, struct Inst *hint, lua_CFunction self, int nup);
// self, nup: the C function now running and its number of upvalues, so that
// hostRun can make it again (no lua_getinfo: lua_Debug's layout is the
// host's, not necessarily our headers')
#define FROM_HOST(L, hint, self, nup)                                         \
    if (lua_touserdata(L, 1) == &GUARD) lua_remove(L, 1);                     \
    else if (stateOf(L)->tc == NULL) return fromHost(L, hint, self, nup)

// The instance of the userdata at idx, if it is a Rexx object's (no error)
static struct Inst *boxInst(lua_State *L, int idx)
{
    RexxBox *b = (RexxBox *)luaL_testudata(L, idx, REXXOBJ);
    return b ? b->inst : NULL;
}

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
    RexxObjectPtr r = NULLOBJECT;
    bool ran = guarded(st, [&] {
        if (message == NULL && c->IsRoutine(target))
            r = c->CallRoutine((RexxRoutineObject)target, args);
        else
            r = c->SendMessage(target, message ? message : "CALL", args);
    });
    if (!ran) { END_THREAD(); return unwindError(L); }   // (no more Rexx calls until it goes on)
    release(c, args);
    if (c->CheckCondition()) {
        pushRexxCondition(L, c);
        END_THREAD();
        return lua_error(L);
    }
    if (r == NULLOBJECT) { END_THREAD(); return 0; }
    bool ok = false;
    ran = guarded(st, [&] { ok = pushValue(st, r); });     // (a string method may run)
    if (ran) release(c, r);
    END_THREAD();
    if (!ran) return unwindError(L);
    if (!ok) return lua_error(L);
    return 1;
}

static int box_call(lua_State *L)       // rexxobj(...)
{
    FROM_HOST(L, boxInst(L, 1), box_call, 0);
    RexxBox *b = checkBox(L, 1);
    return rexxCallBack(L, b->obj, NULL, 2);
}

static int box_method(lua_State *L)     // closure from __index: rexxobj:name(...)
{
    FROM_HOST(L, boxInst(L, lua_upvalueindex(1)), box_method, 2);
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
    if (lua_type(L, 2) == LUA_TSTRING && lua_touserdata(L, 1) != &GUARD) {
        checkBox(L, 1);                 // no Rexx code: no need to go through fromHost
        lua_pushvalue(L, 1);
        pushUpper(L, 2, "");
        lua_pushcclosure(L, box_method, 2);
        return 1;
    }
    FROM_HOST(L, boxInst(L, 1), box_index, 0);
    RexxBox *b = checkBox(L, 1);
    lua_settop(L, 2);
    return rexxCallBack(L, b->obj, "[]", 2);
}

static int box_newindex(lua_State *L)   // obj.name = v: NAME=; obj[k] = v: obj[k] = v
{
    FROM_HOST(L, boxInst(L, 1), box_newindex, 0);
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
    FROM_HOST(L, boxInst(L, 1), box_len, 0);
    RexxBox *b = checkBox(L, 1);
    lua_settop(L, 1);
    return rexxCallBack(L, b->obj, "ITEMS", 2);
}

// pairs(obj): through obj~supplier.  upvalue 1: the supplier, as a userdata.
static int pairs_next(lua_State *L)
{
    FROM_HOST(L, boxInst(L, lua_upvalueindex(1)), pairs_next, 1);
    RexxBox *sup = upvalueBox(L);
    USE_THREAD(L);
    RexxThreadContext *c = st->tc;
    RexxSupplierObject sp = (RexxSupplierObject)sup->obj;
    bool available = false;
    if (!guarded(st, [&] { available = c->SupplierAvailable(sp) != 0; })) { END_THREAD(); return unwindError(L); }
    if (!available) {
        bool error = c->CheckCondition();
        if (error) pushRexxCondition(L, c);
        END_THREAD();
        if (error) return lua_error(L);
        return 0;
    }
    RexxObjectPtr index = NULLOBJECT, item = NULLOBJECT;
    if (!guarded(st, [&] { index = c->SupplierIndex(sp); item = c->SupplierItem(sp); c->SupplierNext(sp); })) {
        END_THREAD();
        return unwindError(L);
    }
    if (c->CheckCondition()) { pushRexxCondition(L, c); END_THREAD(); return lua_error(L); }
    bool ok = false;
    if (!guarded(st, [&] { ok = pushValue(st, index) && pushValue(st, item); })) { END_THREAD(); return unwindError(L); }
    release(c, index);
    release(c, item);
    END_THREAD();
    if (!ok) return lua_error(L);
    return 2;
}

static void pushBox(State *st, RexxObjectPtr v);
static struct Inst *instOfContext(RexxThreadContext *c);
static RexxObjectPtr RexxEntry addressLua(RexxExitContext *c, RexxStringObject address, RexxStringObject command,
                                           RexxIORedirectorContext *io);

static int box_pairs(lua_State *L)
{
    FROM_HOST(L, boxInst(L, 1), box_pairs, 0);
    RexxBox *b = checkBox(L, 1);
    USE_THREAD(L);
    RexxThreadContext *c = st->tc;
    RexxObjectPtr sup = NULLOBJECT;
    if (!guarded(st, [&] { sup = c->SendMessage0(b->obj, "SUPPLIER"); })) { END_THREAD(); return unwindError(L); }
    if (c->CheckCondition()) { pushRexxCondition(L, c); END_THREAD(); return lua_error(L); }
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
    FROM_HOST(L, boxInst(L, lua_upvalueindex(1)), routine_closure, 2);
    RexxBox *b = upvalueBox(L);
    bool predicate = lua_toboolean(L, lua_upvalueindex(2));
    int n = rexxCallBack(L, b->obj, NULL, 1);
    if (predicate && n == 1 && !lua_isboolean(L, -1)) {        // (a .Boolean is one already)
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
    b->inst = st->adopted ? instOfContext(st->tc) : NULL;
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
    FROM_HOST(L, boxInst(L, 1), box_tostring, 0);
    RexxBox *b = checkBox(L, 1);
    USE_THREAD(L);                      // its string method may call Lua
    RexxThreadContext *c = st->tc;
    RexxStringObject s = NULLOBJECT;
    bool ok = false;
    if (!guarded(st, [&] { ok = stringValue(c, b->obj, s); })) { END_THREAD(); return unwindError(L); }
    if (!ok) {
        if (c->CheckCondition()) pushRexxCondition(L, c);
        else lua_pushstring(L, "the Rexx object has no string value");
    }
    else {
        lua_pushlstring(L, c->StringData(s), c->StringLength(s));
        if ((RexxObjectPtr)s != b->obj) release(c, s);
    }
    END_THREAD();
    if (!ok) return lua_error(L);
    return 1;
}

static int box_gc(lua_State *L)
{
    RexxBox *b = (RexxBox *)luaL_checkudata(L, 1, REXXOBJ);
    if (b->obj) {
        State *st = stateOf(L);
        // later (releasePending) when no Rexx code is running, or while a SIGNAL
        // unwinds (guarded): then no Rexx call may run, and Lua's error, as it
        // unwinds, can run finalizers (it crashed on macOS)
        if (st->tc != NULL && !st->unwind) keepRemove(st, b->obj);
        else if (!st->closed) st->pending.push_back(b->obj);
    }
    b->obj = NULL;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Opening and closing a state                                                */
/* ------------------------------------------------------------------------ */

/*
 * C modules built for Lua (LuaRocks's) on Linux and macOS are not linked
 * against liblua: they expect the lua_* symbols to be global.  ooRexx loads
 * this package with RTLD_LOCAL, so the liblua it brought in (a shared one, or
 * the static one linked into this package) is not; reopening it with
 * RTLD_NOLOAD | RTLD_GLOBAL makes it global (docs/design.md, *C modules*).
 * On Windows, C modules are linked against the Lua DLL by name, the same
 * luaXY.dll this package uses: nothing to do.
 */
static void makeLuaGlobal()
{
#ifndef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        Dl_info info;
        if (dladdr((void *)&lua_newstate, &info) && info.dli_fname)
            dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
    });
#endif
}

// What the bridge needs in a Lua state, its own or a host's: its State in the
// registry, the metatable of Rexx objects, and the cache of their userdata
static void setupLua(lua_State *L, State *st)
{
    lua_pushlightuserdata(L, st);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &STATEKEY);
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
}

/* ------------------------------------------------------------------------ */
/* Ctrl-C (docs/design.md, *Ctrl-C*)                                          */
/* ------------------------------------------------------------------------ */

/*
 * ooRexx's handler (SIGINT, SIGTERM, SIGHUP; Ctrl-C on Windows) marks the
 * running Rexx activation halted, and the HALT comes at its next clause
 * boundary.  While Lua code runs, that activation is the one that called into
 * Lua, which does not return: the HALT would wait for ever, and a second
 * signal is ignored while one is pending.  So, as lua.c does, a handler run
 * before ooRexx's marks every state with Lua code running interrupted and
 * sets a hook on its Lua threads (lua_sethook may be called from a signal
 * handler).  The hook raises an error at every instruction, so that no pcall
 * in the Lua code can keep it going.  Every bridge call that returns while
 * its state is interrupted makes its caller's HALT pending itself
 * (HaltThread, takeInterrupt): when the signal came as a callback was
 * ending, ooRexx's mark fell on an activation about to go.  The state stays
 * interrupted until its outermost call returns.  No hook at all otherwise:
 * in Lua 5.4 any hook sends every instruction through the slow path.  Signals
 * that were ignored stay ignored.
 *
 * Every State ever opened is on a list (they are never freed), pushed with a
 * compare-and-swap; the handler only reads it.  Only in a process where Rexx
 * is the host: not when the Lua module made the interpreter instance (the
 * host, LuaTeX say, has its own idea of a Ctrl-C).
 */
static std::atomic<State *> allStates(NULL);

// Stops the Lua code while its state is interrupted; once it is not, it takes
// itself off (a coroutine it was set on may be gone by then, so nothing else
// touches it)
static void stopHook(lua_State *L, lua_Debug *)
{
    State *st = stateOf(L);
    if (st == NULL || !st->interrupted.load()) { lua_sethook(L, NULL, 0, 0); return; }
    luaL_error(L, "interrupted");
}

// From the handler: stop the Lua code now running, on every thread it may be
// running on: the main one (the outer call runs there), the one the bridge
// last entered or left, and every coroutine being resumed
static void stopAll()
{
    const int mask = LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT;
    for (State *st = allStates.load(); st != NULL; st = st->nextAll) {
        if (st->running.load() <= 0 || st->interrupted.load()) continue;
        lua_State *L = st->L, *main = st->main;
        if (L == NULL || main == NULL) continue;
        st->interrupted.store(true);
        lua_sethook(main, stopHook, mask, 1);
        if (L != main) lua_sethook(L, stopHook, mask, 1);
        int n = st->coDepth.load(std::memory_order_acquire);
        for (int i = 0; i < n && i < MAXCO; i++) {
            lua_State *co = st->coStack[i].load(std::memory_order_acquire);
            if (co != NULL) lua_sethook(co, stopHook, mask, 1);
        }
    }
}

/*
 * coroutine.resume and coroutine.wrap, in the states the bridge opens where
 * Rexx is the host: Lua's own (lcorolib.c, of each version), except that the
 * coroutine being resumed is on coStack while lua_resume runs (nested
 * resumes are nested C calls, so a stack), so that a Ctrl-C stops a loop
 * inside a coroutine too.  lua_resume does not raise errors, so the entry
 * always goes.  Not calling Lua's own functions keeps their cost and their
 * depth of C calls (one more level would halve how deep coroutines nest).
 * Lua 5.4 before 5.4.4 closed a wrap's coroutine otherwise: its wrap stays.
 */
// Only the thread inside the state writes them.  Depth d goes in slot d, or
// the last one past it (its previous entry, saved by the caller, comes back
// with coPop): the innermost coroutine is the one running Lua code.
static lua_State *coPush(State *st, lua_State *co, int *slot)
{
    int d = st->coDepth.load(std::memory_order_relaxed);
    *slot = d < MAXCO ? d : MAXCO - 1;
    lua_State *previous = st->coStack[*slot].load(std::memory_order_relaxed);
    st->coStack[*slot].store(co, std::memory_order_release);
    st->coDepth.store(d + 1, std::memory_order_release);
    return previous;
}

static void coPop(State *st, int slot, lua_State *previous)
{
    st->coDepth.store(st->coDepth.load(std::memory_order_relaxed) - 1, std::memory_order_release);
    st->coStack[slot].store(previous, std::memory_order_release);
}

// When the outermost call into the state ends: nothing is being resumed (an
// out-of-memory error in lua_resume can skip a coPop)
static void coClear(State *st)
{
    st->coDepth.store(0, std::memory_order_release);
    for (int i = 0; i < MAXCO; i++) st->coStack[i].store(NULL, std::memory_order_relaxed);
}

// lcorolib.c's auxresume, with co on coStack while it runs
static int auxresume(lua_State *L, State *st, lua_State *co, int narg)
{
    int status, nres, slot = 0;
    if (!lua_checkstack(co, narg)) {
        lua_pushliteral(L, "too many arguments to resume");
        return -1;
    }
#if LUA_VERSION_NUM < 504
    if (lua_status(co) == LUA_OK && lua_gettop(co) == 0) {
        lua_pushliteral(L, "cannot resume dead coroutine");
        return -1;
    }
#endif
    lua_xmove(L, co, narg);
    lua_State *previous = st != NULL ? coPush(st, co, &slot) : NULL;
#if LUA_VERSION_NUM < 504
    status = lua_resume(co, L, narg);
    nres = lua_gettop(co);
#else
    status = lua_resume(co, L, narg, &nres);
#endif
    if (st != NULL) coPop(st, slot, previous);
    if (status == LUA_OK || status == LUA_YIELD) {
        if (!lua_checkstack(L, nres + 1)) {
            lua_pop(co, nres);
            lua_pushliteral(L, "too many results to resume");
            return -1;
        }
        lua_xmove(co, L, nres);
        return nres;
    }
    lua_xmove(co, L, 1);                        // the error
    return -1;
}

// The State of a thread of a state these functions are in: in Lua's extra
// space, which is the bridge's in a state it opened, and which every thread
// copies from the main one (lua_newthread).  Faster than stateOf, which
// would cost a resume a third more; not an upvalue, which Lua's functions do
// not have (and debug.setupvalue could change).
static State *coState(lua_State *L) { return *(State **)lua_getextraspace(L); }

// coroutine.resume
static int co_resume(lua_State *L)
{
    lua_State *co = lua_tothread(L, 1);
#if LUA_VERSION_NUM < 504
    luaL_argcheck(L, co, 1, "thread expected");
#else
    luaL_argexpected(L, co, 1, "thread");
#endif
    int r = auxresume(L, coState(L), co, lua_gettop(L) - 1);
    if (r < 0) {
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_insert(L, -(r + 1));
    return r + 1;
}

// (5.4.0 to 5.4.3 all say 50400: 5.4.3 has the later wrap, 5.4.0 not)
#if LUA_VERSION_NUM < 504 || LUA_VERSION_RELEASE_NUM >= 50404
#define TRACK_WRAP 1
// A wrap's function; upvalue: the coroutine, as Lua's
static int co_auxwrap(lua_State *L)
{
    lua_State *co = lua_tothread(L, lua_upvalueindex(1));
    int r = auxresume(L, coState(L), co, lua_gettop(L));
    if (r < 0) {
#if LUA_VERSION_NUM < 504
        if (lua_type(L, -1) == LUA_TSTRING) {
#else
        int stat = lua_status(co);
        if (stat != LUA_OK && stat != LUA_YIELD) {    // an error in the coroutine:
#if LUA_VERSION_RELEASE_NUM >= 50406                  // close its tbc variables
            stat = lua_closethread(co, L);
#elif LUA_VERSION_RELEASE_NUM == 50405
            stat = lua_resetthread(co, L);
#else
            stat = lua_resetthread(co);
#endif
            lua_xmove(co, L, 1);
        }
        if (stat != LUA_ERRMEM && lua_type(L, -1) == LUA_TSTRING) {
#endif
            luaL_where(L, 1);
            lua_insert(L, -2);
            lua_concat(L, 2);
        }
        return lua_error(L);
    }
    return r;
}

// coroutine.wrap
static int co_wrap(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    lua_pushcclosure(L, co_auxwrap, 1);
    return 1;
}
#endif

static void trackCoroutines(lua_State *L, State *st)
{
    *(State **)lua_getextraspace(L) = st;      // before any other thread is made
    lua_getglobal(L, "coroutine");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, co_resume);
        lua_setfield(L, -2, "resume");
#ifdef TRACK_WRAP
        lua_pushcfunction(L, co_wrap);
        lua_setfield(L, -2, "wrap");
#endif
    }
    lua_pop(L, 1);
}

#ifdef _WIN32
// Called before ooRexx's (handlers run last registered first); FALSE: on to it
static BOOL WINAPI onCtrl(DWORD type)
{
    if (type == CTRL_C_EVENT) stopAll();
    return FALSE;
}
#else
static const int haltSignals[] = { SIGINT, SIGTERM, SIGHUP };
static struct sigaction oldActions[3];
static void onSignal(int sig, siginfo_t *info, void *uc)
{
    stopAll();
    for (int i = 0; i < 3; i++) {
        if (haltSignals[i] != sig) continue;
        struct sigaction *old = &oldActions[i];
        if (old->sa_flags & SA_SIGINFO) { if (old->sa_sigaction != NULL) old->sa_sigaction(sig, info, uc); }
        else if (old->sa_handler == SIG_DFL) { sigaction(sig, old, NULL); raise(sig); }
        else if (old->sa_handler != SIG_IGN) old->sa_handler(sig);
    }
}
#endif

static void installHandler()
{
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        SetConsoleCtrlHandler(onCtrl, TRUE);
#else
        for (int i = 0; i < 3; i++) {
            struct sigaction old;               // an ignored signal stays ignored (nohup)
            if (sigaction(haltSignals[i], NULL, &old) == 0 && !(old.sa_flags & SA_SIGINFO)
                && old.sa_handler == SIG_IGN) continue;
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = onSignal;
            sa.sa_flags = SA_SIGINFO;           // no SA_RESTART, as ooRexx: a read is interrupted
            sigfillset(&sa.sa_mask);
            sigaction(haltSignals[i], &sa, &oldActions[i]);
        }
#endif
    });
}

// After the bridge's lua_pcall: whether a signal stopped its Lua code.  It
// stays stopped until the outer call returns: a nested one (from a callback)
// must not let the outer Lua code go on.  Then the hook of the main thread
// goes now (the others' when they next run, stopHook).
static bool takeInterrupt(State *st)
{
    if (!st->interrupted.load()) return false;
    if (st->running.load() == 0) {
        st->interrupted.store(false);
        if (st->main != NULL) lua_sethook(st->main, NULL, 0, 0);
    }
    return true;
}

static State *openState(RexxThreadContext *c)
{
    makeLuaGlobal();
    State *st = new State();
    lua_State *L = luaL_newstate();
    if (L == NULL) { delete st; return NULL; }
    st->L = st->main = L;
    RexxObjectPtr keep = c->SendMessage0(c->FindClass("IDENTITYTABLE"), "NEW");
    st->keep = c->RequestGlobalReference(keep);       // one per state, never released (#2100)
    luaL_openlibs(L);
    setupLua(L, st);
    // require "oorexx" in it: the instance whose Rexx code is running (no
    // second copy of this library)
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_pushcfunction(L, luaopen_oorexx);
    lua_setfield(L, -2, "oorexx");
    lua_pop(L, 1);
    st->nextAll = allStates.load();
    while (!allStates.compare_exchange_weak(st->nextAll, st)) {}
    if (instOfContext(c) == NULL) { installHandler(); trackCoroutines(L, st); }
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
    RexxClassObject cls = luaObjectClass;
    RexxPointerObject ptr = c->NewPointer(p);
    RexxObjectPtr o = c->SendMessage2(cls, "NEW", ptr, st->self);
    release(c, ptr);                                // the proxy holds it
    if (o == NULLOBJECT) {                          // a condition is pending
        luaL_unref(st->L, LUA_REGISTRYINDEX, p->ref);
        delete p;
        return c->Nil();
    }
    return o;
}

/* .Boolean (boolean.cls, shared by the ooRexx bridges: docs/design.md,
 * *Values*), which lua.cls puts in .local as LUA.BOOLEAN when it loads (the
 * bridge's own name: .environment's BOOLEAN could be anything).  One class
 * per process, also with several instances of the Lua module.  A
 * local reference, or NULLOBJECT where lua.cls is not loaded.             */
static RexxObjectPtr booleanClass(RexxThreadContext *c)
{
    RexxDirectoryObject local = c->GetLocalEnvironment();
    if (local == NULLOBJECT) return NULLOBJECT;
    RexxObjectPtr cls = c->DirectoryAt(local, "LUA.BOOLEAN");
    release(c, local);
    if (cls == NULLOBJECT || cls == c->Nil()) return NULLOBJECT;
    return cls;
}

// Is v a .Boolean (or of a subclass)?  Its logical value in *value.
static bool isBoolean(RexxThreadContext *c, RexxObjectPtr v, bool *value)
{
    RexxObjectPtr cls = booleanClass(c);
    if (cls == NULLOBJECT) return false;
    bool is = c->IsInstanceOf(v, (RexxClassObject)cls) != 0;
    release(c, cls);
    if (!is) return false;
    logical_t b = 0;
    if (!c->ObjectToLogical(v, &b)) { c->ClearCondition(); return false; }
    *value = b != 0;
    return true;
}

static NOINLINE RexxObjectPtr toRexx(State *st, int idx)
{
    RexxThreadContext *c = st->tc;
    lua_State *L = st->L;
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:     return c->Nil();
    case LUA_TBOOLEAN: {                    // .Boolean~true, .Boolean~false (*Values*)
        RexxObjectPtr cls = booleanClass(c);
        if (cls == NULLOBJECT) return lua_toboolean(L, idx) ? c->True() : c->False();
        RexxObjectPtr b = c->SendMessage0(cls, lua_toboolean(L, idx) ? "TRUE" : "FALSE");
        release(c, cls);
        return b != NULLOBJECT ? b : (lua_toboolean(L, idx) ? c->True() : c->False());
    }
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
    if (!c->HasMethod(v, "LUA.REF")) return NULL;     // not by class name: see luaObjectClass
    RexxObjectPtr p = c->SendMessage0(v, "LUA.REF");
    return p == NULLOBJECT ? NULL : (Proxy *)c->PointerValue((RexxPointerObject)p);
}

/* Rexx Array -> a new table (docs/design.md, *Values*).  One dimension: a
 * sequence, with the field n = the Array's size when some slot in 1..size
 * is nil in Lua (empty, or .nil), since #t cannot tell the size then.  More
 * dimensions: nested sequences, the first dimension outermost, each row by
 * the same rule (an outer table has no holes: its rows are tables).  An
 * Array keeps its items with the FIRST index varying fastest (measured:
 * m[2,1] is at linear position 2 of a 2x3), so the item at (i1, ..., ik) is
 * the one at linear position 1 + sum((ij - 1) * stride(j)), with stride(1)
 * = 1 and stride(j) = stride(j-1) * dim(j-1).
 *
 * One table per Array: while one value is converted, a table on Lua's stack
 * (memo, its index; 0 before the first Array) maps each Array already
 * converted to its table, so an Array that contains itself, or appears
 * twice, is the same table again (t[3] == t in Lua for a~append(a)), as it
 * is the same object in Rexx.  The memo lives on the stack, not in the
 * State, so an error that unwinds the conversion leaves nothing behind.
 * Nesting deeper than MAX_ARRAY_DEPTH Arrays is an error: the conversion
 * recurses in C.                                                          */
enum { MAX_ARRAY_DIMS = 32, MAX_ARRAY_DEPTH = 1000 };
static const lua_Integer MAX_MAKEARRAY = 100000000;     // ~makeArray's limit

static bool pushArrayLevel(State *st, RexxArrayObject a, const size_t *dims, size_t ndims,
                           size_t level, size_t offset, const size_t *strides, int memo, int depth)
{
    RexxThreadContext *c = st->tc;
    lua_State *L = st->L;
    if (!lua_checkstack(L, 4)) { lua_pushstring(L, "an Array nested too deeply"); return false; }
    size_t count = dims[level];
    if (level + 1 < ndims) {
        lua_createtable(L, (int)count, 0);
        if (level == 0) { lua_pushvalue(L, -1); lua_rawsetp(L, memo, a); }
        for (size_t i = 1; i <= count; i++) {
            if (!pushArrayLevel(st, a, dims, ndims, level + 1, offset + (i - 1) * strides[level], strides, memo, depth)) {
                lua_remove(L, -2); return false;
            }
            lua_rawseti(L, -2, (lua_Integer)i);
        }
        return true;
    }
    lua_createtable(L, (int)count, 0);
    if (level == 0) { lua_pushvalue(L, -1); lua_rawsetp(L, memo, a); }
    bool holes = false;
    for (size_t i = 1; i <= count; i++) {
        RexxObjectPtr item = c->ArrayAt(a, 1 + offset + (i - 1) * strides[level]);
        if (item == NULLOBJECT || item == c->Nil()) { holes = true; continue; }
        bool ok = pushValue(st, item, memo, depth);
        release(c, item);           // the Array keeps it (a million items held 30 MB more)
        if (!ok) { lua_remove(L, -2); return false; }
        lua_rawseti(L, -2, (lua_Integer)i);
    }
    if (holes) {
        lua_pushinteger(L, (lua_Integer)count);
        lua_setfield(L, -2, "n");
    }
    return true;
}

// (Its own frame: the dimensions' arrays are not on the C stack of every
// level of a nested one-dimensional Array.)
static NOINLINE bool pushArrayDims(State *st, RexxArrayObject a, size_t ndims, int memo, int depth)
{
    RexxThreadContext *c = st->tc;
    size_t dims[MAX_ARRAY_DIMS], strides[MAX_ARRAY_DIMS];
    if (ndims > MAX_ARRAY_DIMS) {
        lua_pushstring(st->L, "an Array of more than 32 dimensions");
        return false;
    }
    for (size_t j = 0; j < ndims; j++) {
        RexxObjectPtr idx = c->WholeNumberToObject((wholenumber_t)(j + 1));
        RexxObjectPtr d = c->SendMessage1(a, "DIMENSION", idx);
        release(c, idx);
        size_t size = 0;
        if (d == NULLOBJECT || !c->ObjectToStringSize(d, &size)) {
            if (d != NULLOBJECT) release(c, d);
            c->ClearCondition();
            lua_pushstring(st->L, "an Array whose dimensions cannot be read");
            return false;
        }
        release(c, d);
        dims[j] = size;
    }
    strides[0] = 1;
    for (size_t j = 1; j < ndims; j++) strides[j] = strides[j - 1] * dims[j - 1];
    return pushArrayLevel(st, a, dims, ndims, 0, 0, strides, memo, depth);
}

static NOINLINE bool pushArrayIn(State *st, RexxArrayObject a, int memo, int depth)
{
    lua_State *L = st->L;
    if (lua_rawgetp(L, memo, a) != LUA_TNIL) return true;      // converted already
    lua_pop(L, 1);
    if (depth >= MAX_ARRAY_DEPTH) {
        lua_pushstring(L, "an Array nested more than 1000 deep");
        return false;
    }
    size_t ndims = st->tc->ArrayDimension(a);
    if (ndims > 1) return pushArrayDims(st, a, ndims, memo, depth + 1);
    size_t size = st->tc->ArraySize(a), stride = 1;
    return pushArrayLevel(st, a, &size, 1, 0, 0, &stride, memo, depth + 1);
}

static NOINLINE bool pushArray(State *st, RexxArrayObject a, int memo, int depth)
{
    if (memo != 0) return pushArrayIn(st, a, memo, depth);
    lua_State *L = st->L;                   // the outermost Array: a memo for it
    if (!lua_checkstack(L, 4)) { lua_pushstring(L, "an Array nested too deeply"); return false; }
    lua_newtable(L);
    int m = lua_gettop(L);
    bool ok = pushArrayIn(st, a, m, 0);
    lua_remove(L, m);                       // the table, or the message, stays
    return ok;
}

/* Push a Rexx value.  False if it cannot go to this state (a .LuaObject of
 * another state, or of a closed one): then the error message is pushed
 * instead, for lua_error or for raiseTop.                                  */
static NOINLINE bool pushValue(State *st, RexxObjectPtr v, int memo, int depth)
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
    if (c->IsArray(v)) return pushArray(st, (RexxArrayObject)v, memo, depth);
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
    // A .Boolean: Lua's boolean.  A Routine: a real Lua function.  Any other
    // object: a userdata Lua can send messages to (obj:name(...)).
    bool b;
    if (isBoolean(c, v, &b)) { lua_pushboolean(L, b ? 1 : 0); return true; }
    if (c->IsRoutine(v)) { pushRoutine(st, v, false); return true; }
    pushBox(st, v);
    return true;
}

// After a failed pushValue: raise its message as a Rexx error and pop it (a
// Rexx error, from a string method: that condition again).
static void raiseTop(State *st)
{
    RexxObjectPtr cond = conditionOf(st->L, -1);
    if (cond != NULLOBJECT) { raiseCondition(st->tc, cond); lua_pop(st->L, 1); return; }
    size_t length;
    const char *s = lua_tolstring(st->L, -1, &length);
    std::string m = "Lua error: ";
    if (s != NULL) m.append(s, length); else m += "(error object)";     // a NUL stays
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
        : st(s), enter((drain(m->threadContext), s), m->threadContext, stateObj, foreignThread(s) ? SKIP : LOCK),
          ok(true)
    {
        if (foreignThread(st)) {
            raise(m->threadContext, "Lua error: the LuaState of the Lua that runs ooRexx can only be used "
                                    "from that Lua's thread");
            ok = false;
        }
        else if (st->closed) { raise(m->threadContext, "Lua error: the LuaState is closed"); ok = false; }
        else releasePending(st);
    }
};

/* ------------------------------------------------------------------------ */
/* .LuaState                                                                  */
/* ------------------------------------------------------------------------ */

// Set by Lua_AdoptNext for the LuaState_New that follows it on this thread
static thread_local State *adoptNext = NULL;

// .LuaState~new: its CSELF (a new state, or the host's: Lua_AdoptNext)
RexxMethod0(RexxObjectPtr, LuaState_New)
{
    if (adoptNext != NULL) {
        State *host = adoptNext;
        adoptNext = NULL;
        return context->NewPointer(host);
    }
    drain(context->threadContext);
    State *st = openState(context->threadContext);
    if (st == NULL) { raise(context->threadContext, "Lua error: cannot create a Lua state"); return NULLOBJECT; }
    return context->NewPointer(st);
}

// .LuaObject~graveyard (lua.cls, the class's activate): one Queue for the process
RexxMethod0(RexxObjectPtr, Lua_Graveyard)
{
    std::lock_guard<std::mutex> guard(graveyardLock);
    luaObjectClass = (RexxClassObject)context->RequestGlobalReference(context->GetSelf());
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
    if (st->adopted) {
        raise(context->threadContext, "Lua error: the LuaState of the Lua that runs ooRexx is closed by that Lua, "
                                      "not by Rexx");
        return NULLOBJECT;
    }
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
    if (!protect(en.st, p_getglobal, 1)) return failed(en.st);
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
    if (!protect(en.st, p_getglobal, 1)) return failed(en.st);
    lua_pushstring(en.st->L, name);
    return firstResult(en.st, pcallAll(en.st, 1));
}

// A Rexx string that ObjectToDouble read as d, an infinity or a zero: the
// message if that is a number out of a double's range ("" if it is fine:
// +infinity, -infinity, or a Rexx zero)
static NOINLINE std::string floatRange(const char *p, size_t n, double d)
{
    std::string v(p, n);
    size_t a = v.find_first_not_of(' '), b = v.find_last_not_of(' ');
    std::string w = a == std::string::npos ? "" : v.substr(a, b - a + 1);
    if (std::isinf(d)) {
        if (w == "+infinity" || w == "-infinity") return "";
        return w + " is out of range for a float (too large: it would be " + (d > 0 ? "+" : "-") + "infinity)";
    }
    size_t e = w.find_first_of("eE");
    if (w.substr(0, e).find_first_of("123456789") == std::string::npos) return "";   // a zero
    return w + " is out of range for a float (too small: it would be zero)";
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
    bool whole = false;                 // bad is the whole message (no ", not ...")
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
        // ObjectToDouble reads nan, +infinity, -infinity too: what a float
        // that is not a number becomes in Rexx (topics/float-text.md).  It
        // turns a Rexx string out of a double's range into an infinity or a
        // zero without saying so: that is an error, unless the string asked
        // for the infinity by name (topics/explicit-float-range.md)
        if (c->ObjectToDouble(value, &d)) {
            RexxStringObject s;
            if (c->IsString(value) && (std::isinf(d) || d == 0) && stringValue(c, value, s))
                bad = floatRange(c->StringData(s), c->StringLength(s), d);
            if (bad.empty()) lua_pushnumber(L, (lua_Number)d);
            else whole = true;
        }
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
        if (!whole && bad.compare(0, 8, "the type") != 0 && stringValue(c, value, s))
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

// s~table of an Array subclass: the shape from DIMENSIONS (nested tables, as
// array-shape.md keeps it), the items from SUPPLIER (the stored ones, so an
// own SUPPLIER is honoured), then n in each row with holes.  0: done, the
// table pushed; 1: a message pushed; 2: a Rexx condition is pending
static void shapeLevel(lua_State *L, const size_t *dims, size_t ndims, size_t level)
{
    lua_createtable(L, (int)dims[level], 0);
    if (level + 1 == ndims) return;
    for (size_t i = 1; i <= dims[level]; i++) {
        shapeLevel(L, dims, ndims, level + 1);
        lua_rawseti(L, -2, (lua_Integer)i);
    }
}

static void holesLevel(lua_State *L, const size_t *dims, size_t ndims, size_t level)
{
    if (level + 1 < ndims) {
        for (size_t i = 1; i <= dims[level]; i++) {
            lua_rawgeti(L, -1, (lua_Integer)i);
            holesLevel(L, dims, ndims, level + 1);
            lua_pop(L, 1);
        }
        return;
    }
    for (size_t i = 1; i <= dims[level]; i++) {
        bool hole = lua_rawgeti(L, -1, (lua_Integer)i) == LUA_TNIL;
        lua_pop(L, 1);
        if (hole) {
            lua_pushinteger(L, (lua_Integer)dims[level]);
            lua_setfield(L, -2, "n");
            return;
        }
    }
}

static NOINLINE int pushArraySubclass(State *st, RexxObjectPtr a)
{
    RexxThreadContext *c = st->tc;
    lua_State *L = st->L;
    RexxObjectPtr ds = c->SendMessage0(a, "DIMENSIONS");
    if (c->CheckCondition()) return 2;
    size_t ndims = c->ArrayItems((RexxArrayObject)ds), dims[MAX_ARRAY_DIMS];
    if (ndims > MAX_ARRAY_DIMS) { lua_pushstring(L, "an Array of more than 32 dimensions"); return 1; }
    if (ndims == 0) { lua_newtable(L); return 0; }
    for (size_t j = 0; j < ndims; j++)
        if (!c->ObjectToStringSize(c->ArrayAt((RexxArrayObject)ds, j + 1), &dims[j])) {
            c->ClearCondition();
            lua_pushstring(L, "an Array whose dimensions cannot be read");
            return 1;
        }
    if (!lua_checkstack(L, (int)ndims + 8)) { lua_pushstring(L, "an Array nested too deeply"); return 1; }
    shapeLevel(L, dims, ndims, 0);
    int t = lua_gettop(L);
    RexxSupplierObject sup = (RexxSupplierObject)c->SendMessage0(a, "SUPPLIER");
    if (c->CheckCondition()) return 2;
    while (c->SupplierAvailable(sup)) {
        RexxObjectPtr index = c->SupplierIndex(sup), item = c->SupplierItem(sup);
        size_t pos[MAX_ARRAY_DIMS];
        bool ok = true;
        if (ndims == 1) ok = c->ObjectToStringSize(index, &pos[0]);
        else if (!c->IsArray(index) || c->ArrayItems((RexxArrayObject)index) != ndims) ok = false;
        else for (size_t j = 0; ok && j < ndims; j++)
            ok = c->ObjectToStringSize(c->ArrayAt((RexxArrayObject)index, j + 1), &pos[j]);
        for (size_t j = 0; ok && j < ndims; j++) ok = pos[j] >= 1 && pos[j] <= dims[j];
        if (!ok) {
            c->ClearCondition();
            lua_settop(L, t - 1);
            lua_pushstring(L, "an Array subclass whose SUPPLIER gives an index outside its DIMENSIONS");
            return 1;
        }
        lua_pushvalue(L, t);
        for (size_t j = 0; j + 1 < ndims; j++) { lua_rawgeti(L, -1, (lua_Integer)pos[j]); lua_remove(L, -2); }
        if (item != NULLOBJECT && item != c->Nil()) {
            if (!pushValue(st, item)) { lua_remove(L, -2); lua_remove(L, t); return 1; }
            lua_rawseti(L, -2, (lua_Integer)pos[ndims - 1]);
        }
        lua_pop(L, 1);
        c->SupplierNext(sup);
        if (c->CheckCondition()) { lua_settop(L, t - 1); return 2; }
    }
    holesLevel(L, dims, ndims, 0);
    return 0;
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
    // A subclass is copied as its class is, read as ooRexx enumerates it, by
    // SUPPLIER (what DO WITH ... OVER sees): an own SUPPLIER is honoured, and
    // what it only computes (UNKNOWN) is not copied (oorexx-bridges,
    // topics/directory-crossing.md, the amendment to point 2)
    else if (c->IsDirectory(src) || c->IsStringTable(src) ||
             c->IsInstanceOf(src, c->FindClass("DIRECTORY")) ||
             c->IsInstanceOf(src, c->FindClass("STRINGTABLE"))) {
        lua_newtable(L);
        RexxObjectPtr sup = c->SendMessage0(src, "SUPPLIER");
        if (c->CheckCondition()) { lua_settop(L, top); return NULLOBJECT; }
        while (c->SupplierAvailable((RexxSupplierObject)sup)) {
            if (!pushValue(st, c->SupplierIndex((RexxSupplierObject)sup)) ||
                !pushValue(st, c->SupplierItem((RexxSupplierObject)sup))) {
                raiseTop(st); lua_settop(L, top); return NULLOBJECT;
            }
            lua_settable(L, -3);
            c->SupplierNext((RexxSupplierObject)sup);
        }
    }
    else if (c->IsInstanceOf(src, c->FindClass("ARRAY"))) {
        int r = pushArraySubclass(st, src);
        if (r == 2) { lua_settop(L, top); return NULLOBJECT; }
        if (r == 1) { raiseTop(st); lua_settop(L, top); return NULLOBJECT; }
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
    if (!protect(st, p_resolve, 2)) { lua_settop(L, top); return failed(st); }
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
    if (!protect(st, p_resolve, 2)) return failed(st);
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
    if (!protect(st, p_gettable, 2)) { lua_settop(L, top); return failed(st); }
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
    if (!protect(st, p_tostring, 1)) return c->CheckCondition() ? NULLOBJECT : c->NullString();   // Ctrl-C
    size_t len;
    const char *s = lua_tolstring(L, -1, &len);
    RexxObjectPtr r = c->NewString(s, len);
    lua_pop(L, 1);
    return r;
}

// After Ctrl-C (see failed) an empty Array, so that the loop ends first
static RexxObjectPtr emptyArray(RexxThreadContext *c)
{
    return c->CheckCondition() ? NULLOBJECT : (RexxObjectPtr)c->NewArray(0);
}

// do x over o: the sequence part (1..#o)
RexxMethod1(RexxObjectPtr, LuaObj_MakeArray, CSELF, self)
{
    PROXY_ENTRY
    int top = lua_gettop(L);
    pushSelf(px);
    int obj = lua_gettop(L);
    lua_pushvalue(L, obj);
    if (!protect(st, p_size, 1)) { lua_settop(L, top); return emptyArray(c); }
    lua_Integer n = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (n > MAX_MAKEARRAY) {            // t.n or __len: NewArray would not survive it
        lua_settop(L, top);
        raise(c, "a Lua table of more than 100000000 items cannot become an Array");
        return NULLOBJECT;
    }
    RexxArrayObject a = c->NewArray(n > 0 ? (size_t)n : 0);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_pushvalue(L, obj);
        lua_pushinteger(L, i);
        if (!protect(st, p_gettable, 2)) { lua_settop(L, top); return emptyArray(c); }
        if (!lua_isnil(L, -1)) c->ArrayPut(a, toRexx(st, -1), (size_t)i);   // nil: an empty slot
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return a;
}

// ~supplier: Lua's pairs (every key and value, by the usual rules, in Lua's
// order, __pairs honoured), so that DO WITH INDEX k ITEM v OVER t works
// (oorexx-bridges, topics/positions.md, *Related, Lua only*)
RexxMethod1(RexxObjectPtr, LuaObj_Supplier, CSELF, self)
{
    PROXY_ENTRY
    int top = lua_gettop(L);
    pushSelf(px);
    if (!protect(st, p_pairs, 1)) {
        lua_settop(L, top);
        return c->CheckCondition() ? NULLOBJECT : (RexxObjectPtr)c->NewSupplier(c->NewArray(0), c->NewArray(0));
    }
    lua_Integer n = lua_tointeger(L, -1);
    int keys = lua_gettop(L) - 2, values = keys + 1;
    RexxArrayObject ks = c->NewArray((size_t)n), vs = c->NewArray((size_t)n);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, keys, i);
        c->ArrayPut(ks, toRexx(st, -1), (size_t)i);
        lua_pop(L, 1);
        lua_rawgeti(L, values, i);
        c->ArrayPut(vs, lua_isnil(L, -1) ? c->Nil() : toRexx(st, -1), (size_t)i);
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return c->NewSupplier(vs, ks);
}

// ~makeStringTable: the string keys only, as stored (case kept), values by
// the usual rules (a table by reference); other keys are left out, as
// makeArray leaves out what is not a position (topics/positions.md)
RexxMethod1(RexxObjectPtr, LuaObj_MakeStringTable, CSELF, self)
{
    PROXY_ENTRY
    int top = lua_gettop(L);
    pushSelf(px);
    RexxStringTableObject r = c->NewStringTable();
    if (!protect(st, p_pairs, 1)) { lua_settop(L, top); return c->CheckCondition() ? NULLOBJECT : (RexxObjectPtr)r; }
    lua_Integer n = lua_tointeger(L, -1);
    int keys = lua_gettop(L) - 2, values = keys + 1;
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, keys, i);
        lua_rawgeti(L, values, i);
        if (lua_type(L, -2) == LUA_TSTRING && !lua_isnil(L, -1)) {
            size_t len;
            const char *k = lua_tolstring(L, -2, &len);
            c->SendMessage2(r, "PUT", toRexx(st, -1), c->NewString(k, len));
        }
        lua_pop(L, 2);
    }
    lua_settop(L, top);
    return r;
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
    if (!protect(st, e->kind == OP_ARITH ? p_arith : p_compare, unary ? 2 : 3)) {
        lua_settop(L, top);                     // after Ctrl-C a comparison is false (an IF
        RexxObjectPtr r = failed(st);           // or WHILE needs a logical value)
        return r == c->Nil() && e->kind == OP_COMPARE ? c->False() : r;
    }
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
        else {                      // the double's bits: no locale in them (%a has its point)
            double d = (double)lua_tonumber(L, -1);
            uint64_t bits;
            memcpy(&bits, &d, sizeof bits);
            snprintf(buf, sizeof buf, "f%016llx", (unsigned long long)bits);
        }
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
/* Lua -> ooRexx: the Lua module "oorexx" (docs/design.md, *Lua → ooRexx*)     */
/* ------------------------------------------------------------------------ */

/*
 *   local rexx = require "oorexx"
 *   rexx.eval(expr, args...)       the value of a Rexx expression (args: arg(1) ...)
 *   rexx.run(program, args...)     CALL a program: its result
 *   rexx.routine(source[, name])   a Rexx routine, as a Lua function
 *   rexx.interpreter{...}          another instance, with options: a table with
 *                                  eval, run, routine and close
 *
 * In a host's Lua (lua, texlua ...), the module adopts the host's lua_State:
 * it is the .lua of every instance the module makes, and the module's
 * functions work on a default instance, made at their first use.  The module
 * registers this library with each instance as "rexxlua", so lua.cls finds it
 * without loading librexxlua.  In a state the bridge made (Rexx started, or
 * .LuaState~new), require "oorexx" is package.preload's (openState): its
 * functions work on the instance whose Rexx code is running.
 */

extern RexxPackageEntry rexxlua_package_entry;

/* ------------------------------------------------------------------------ */
/* Command environments written in Lua (docs/design.md, *Command environments*) */
/* ------------------------------------------------------------------------ */

/*
 * ADDRESS name runs a Lua callable, handler(command, ctx), in its own state.
 * One native handler, luaCommand, serves every name.  The names of an
 * instance are in its .local, LUA.COMMANDS: a StringTable, name (uppercase)
 * -> the handler (a .LuaObject), or .nil once removed.  A name in it has
 * been given to ooRexx (AddCommandEnvironment, or REDIRECTING_ENVIRONMENTS
 * when the module made the instance) and is never given again: a name
 * removed stays there, and its commands answer FAILURE RC 30, as an
 * unknown environment does (ooRexx has no API that removes one).
 * oorexx-bridges, topics/command-handlers.md (decided 19/10/2026).
 */
static std::mutex commandsLock;             // a name's test and its AddCommandEnvironment, together
static const char *COMMANDS = "LUA.COMMANDS";

static std::string upperString(const char *s, size_t n)
{
    std::string r(s, n);
    for (char &ch : r) ch = (char)toupper((unsigned char)ch);
    return r;
}

// The names of the instance of tc (made if make), or NULLOBJECT
static RexxObjectPtr commandsOf(RexxThreadContext *tc, bool make)
{
    RexxDirectoryObject local = tc->GetLocalEnvironment();
    if (local == NULLOBJECT) return NULLOBJECT;
    RexxObjectPtr t = tc->DirectoryAt(local, COMMANDS);
    if ((t == NULLOBJECT || t == tc->Nil()) && make) {
        t = tc->SendMessage0(tc->FindClass("STRINGTABLE"), "NEW");
        if (t != NULLOBJECT) tc->DirectoryPut(local, t, COMMANDS);
    }
    return t == tc->Nil() ? NULLOBJECT : t;
}

static bool hasIndex(RexxThreadContext *tc, RexxObjectPtr t, const std::string &name)
{
    RexxObjectPtr r = tc->SendMessage1(t, "HASINDEX", tc->String(name.c_str()));
    logical_t b = 0;
    return r != NULLOBJECT && tc->ObjectToLogical(r, &b) && b;
}

// An incorrect argument of a command environment method: SYNTAX 93.900
// (98.900 is for errors that come from Lua; command-handlers.md, rule 4)
static void argError(RexxThreadContext *tc, const std::string &m)
{
    tc->RaiseException1(Rexx_Error_Incorrect_method_user_defined, tc->NewString(m.data(), m.size()));
}

// The name of an environment, uppercase (what ADDRESS name gives); false
// after raising an error
static NOINLINE bool environmentName(RexxThreadContext *tc, RexxObjectPtr o, const char *what, std::string &name)
{
    if (o == NULLOBJECT || !(tc->IsString(o) || tc->IsOfType(o, "STRING"))) {
        argError(tc, std::string(what) + ": the name of the environment must be a string");
        return false;
    }
    RexxStringObject s;
    if (!stringValue(tc, o, s)) return false;
    name = upperString(tc->StringData(s), tc->StringLength(s));
    if (name.empty()) { argError(tc, std::string(what) + ": the name of the environment is empty"); return false; }
    if (name == "LUA") { argError(tc, std::string(what) + ": LUA is the bridge's own environment (ADDRESS LUA)"); return false; }
    return true;
}

static RexxObjectPtr RexxEntry luaCommand(RexxExitContext *c, RexxStringObject address,
                                           RexxStringObject command, RexxIORedirectorContext *io);

// Is the value at idx something Lua can call: a function, or a value with __call?
static bool isCallable(lua_State *L, int idx)
{
    if (lua_type(L, idx) == LUA_TFUNCTION) return true;
    if (luaL_getmetafield(L, idx, "__call") == LUA_TNIL) return false;
    lua_pop(L, 1);
    return true;
}

/* addCommandEnvironment(name, handler): name is served by handler, a
 * .LuaObject of a callable, from now on; a name already there gets the new
 * handler.  False after raising an error (no handler, .nil included, is
 * one: removing is removeEnvironment).                                     */
static NOINLINE bool addEnvironment(RexxThreadContext *tc, RexxObjectPtr nameObj, RexxObjectPtr handler,
                                    const char *what)
{
    std::string name;
    if (!environmentName(tc, nameObj, what, name)) return false;
    Proxy *px = handler == NULLOBJECT || handler == tc->Nil() ? NULL : proxyOf(tc, handler);
    if (px == NULL) {
        argError(tc, std::string(what) + ": the handler must be a Lua function, or a Lua value with __call"
                 + (handler == NULLOBJECT || handler == tc->Nil() ? " (to remove an environment: removeCommandEnvironment)" : ""));
        return false;
    }
    State *st = px->st;
    if (foreignThread(st)) { raise(tc, std::string(what) + ": " + noRexx(st)); return false; }
    if (st->closed) { raise(tc, std::string(what) + ": the handler's LuaState is closed"); return false; }
    bool callable;
    {
        Enter e(st, tc, NULLOBJECT);
        lua_State *L = st->L;
        if (!lua_checkstack(L, 3)) { raise(tc, "Lua error: Lua stack overflow"); return false; }
        pushSelf(px);
        callable = isCallable(L, -1);
        lua_pop(L, 1);
    }
    if (!callable) { argError(tc, std::string(what) + ": the handler must be a Lua function, or a Lua value with __call"); return false; }
    std::lock_guard<std::mutex> guard(commandsLock);
    RexxObjectPtr t = commandsOf(tc, true);
    if (t == NULLOBJECT) return false;
    if (!hasIndex(tc, t, name))
        tc->AddCommandEnvironment(name.c_str(), (REXXPFN)luaCommand, REDIRECTING_COMMAND_ENVIRONMENT);
    tc->SendMessage2(t, "PUT", handler, tc->String(name.c_str()));
    return !tc->CheckCondition();
}

/* removeCommandEnvironment(name): its commands answer FAILURE RC 30 from
 * now on.  A name with no handler (never added, or removed already) is an
 * error (rule 4 of command-handlers.md, the user, 19/10/2026): *missing,
 * for the caller to raise in its host's way.  False after an error.       */
static NOINLINE bool removeEnvironment(RexxThreadContext *tc, RexxObjectPtr nameObj, const char *what, bool *missing)
{
    *missing = false;
    std::string name;
    if (!environmentName(tc, nameObj, what, name)) return false;
    std::lock_guard<std::mutex> guard(commandsLock);
    RexxObjectPtr t = commandsOf(tc, false);
    RexxObjectPtr h = t == NULLOBJECT ? NULLOBJECT : tc->SendMessage1(t, "AT", tc->String(name.c_str()));
    if (h == NULLOBJECT || h == tc->Nil()) { *missing = !tc->CheckCondition(); return !tc->CheckCondition(); }
    tc->SendMessage2(t, "PUT", tc->Nil(), tc->String(name.c_str()));
    return !tc->CheckCondition();
}

// .LuaState~addCommandEnvironment(name, handler), ~removeCommandEnvironment(name):
// for the instance this runs in (any .LuaState: the handler's own state runs it)
RexxMethod2(RexxObjectPtr, LuaState_AddCommandEnvironment, OPTIONAL_RexxObjectPtr, name, OPTIONAL_RexxObjectPtr, handler)
{
    drain(context->threadContext);
    addEnvironment(context->threadContext, name, handler, "addCommandEnvironment");
    return NULLOBJECT;
}

RexxMethod1(RexxObjectPtr, LuaState_RemoveCommandEnvironment, OPTIONAL_RexxObjectPtr, name)
{
    drain(context->threadContext);
    bool missing;
    if (removeEnvironment(context->threadContext, name, "removeCommandEnvironment", &missing) && missing) {
        argError(context->threadContext, std::string("removeCommandEnvironment: ") + context->CString(name) + " has no handler");
    }
    return NULLOBJECT;
}

/* A command of a Lua handler, while it runs.  Its ctx (a table) carries its
 * number: once it has ended, the ctx finds no Command, and its methods raise
 * an error.                                                                */
struct Command {
    RexxExitContext *c = NULL;
    RexxIORedirectorContext *io = NULL;
    lua_Integer id = 0;
    Command *prev = NULL;
    const char *condition = NULL;           // "ERROR", "FAILURE": asked for by raise_error, raise_failure
    RexxObjectPtr rc = NULLOBJECT;          // their rc, if they had one
};

static char CTXID;                          // key, in a ctx, of its command's number
static char CTXMETA;                        // registry: the metatable of the ctx tables

// The Command of the ctx at 1, and its state; raises a Lua error if there is none
static Command *commandOf(lua_State *L, State **pst)
{
    lua_Integer id = 0;
    if (lua_type(L, 1) == LUA_TTABLE) {
        lua_rawgetp(L, 1, &CTXID);
        id = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
    }
    if (id == 0) luaL_argerror(L, 1, "a command's context expected (ctx:name(...))");
    State *st = stateOf(L);
    Command *cmd = NULL;
    if (st != NULL) for (cmd = st->command; cmd != NULL && cmd->id != id; cmd = cmd->prev) {}
    if (cmd == NULL) luaL_error(L, "this command context belongs to a command that has ended");
    *pst = st;
    return cmd;
}

static bool inputRedirected(Command *cmd) { return cmd->io != NULL && cmd->io->IsInputRedirected(); }

// After a Rexx call in a ctx method (st->L was set to L): st->L back, and a
// Rexx condition raised as a Lua error
#define CTX_CHECK(L, st, outerL)                                              \
    do { (st)->L = (outerL);                                                  \
         if ((st)->unwind) return unwindError(L);                             \
         if ((st)->tc->CheckCondition()) { pushRexxCondition((L), (st)->tc); return lua_error(L); } } while (0)

static int ctx_line_in(lua_State *L)        // ctx:line_in(): the next input line, nil at the end
{
    State *st;
    Command *cmd = commandOf(L, &st);
    if (!inputRedirected(cmd)) { lua_pushnil(L); return 1; }
    CSTRING data = NULL;
    size_t len = 0;
    lua_State *outerL = st->L;
    st->L = L;                              // a USING source may call back into Lua
    guarded(st, [&] { cmd->io->ReadInput(&data, &len); });
    CTX_CHECK(L, st, outerL);
    if (data == NULL) lua_pushnil(L); else lua_pushlstring(L, data, len);
    return 1;
}

static int ctx_lines(lua_State *L)          // ctx:lines(): the rest of the input, a sequence
{
    State *st;
    Command *cmd = commandOf(L, &st);
    lua_newtable(L);
    if (!inputRedirected(cmd)) return 1;
    int t = lua_gettop(L);
    for (lua_Integer i = 1; ; i++) {
        CSTRING data = NULL;
        size_t len = 0;
        lua_State *outerL = st->L;
        st->L = L;
        guarded(st, [&] { cmd->io->ReadInput(&data, &len); });
        CTX_CHECK(L, st, outerL);
        if (data == NULL) break;
        lua_pushlstring(L, data, len);
        lua_rawseti(L, t, i);
    }
    return 1;
}

// ctx:output(text), ctx:error(text): a line, to the command's output (error)
// if it is redirected, else to .output (.error), as a real command's console
// output (rule 2 of command-handlers.md: never dropped)
static int ctxWrite(lua_State *L, bool err)
{
    State *st;
    Command *cmd = commandOf(L, &st);
    luaL_checkany(L, 2);
    size_t len;
    const char *s = luaL_tolstring(L, 2, &len);
    RexxThreadContext *tc = st->tc;
    lua_State *outerL = st->L;
    st->L = L;                              // a USING target may call back into Lua
    bool redirected = cmd->io != NULL && (err ? cmd->io->IsErrorRedirected() : cmd->io->IsOutputRedirected());
    if (redirected) {
        guarded(st, [&] { if (err) cmd->io->WriteError(s, len); else cmd->io->WriteOutput(s, len); });
    }
    else guarded(st, [&] {                  // (released: many lines in one command must not grow)
        RexxDirectoryObject local = tc->GetLocalEnvironment();
        RexxObjectPtr stream = local != NULLOBJECT ? tc->DirectoryAt(local, err ? "ERROR" : "OUTPUT") : NULLOBJECT;
        if (stream != NULLOBJECT && stream != tc->Nil()) {
            RexxStringObject line = tc->NewString(s, len);
            release(tc, tc->SendMessage1(stream, "LINEOUT", line));
            release(tc, line);
        }
        release(tc, stream);
        release(tc, local);
    });
    CTX_CHECK(L, st, outerL);
    return 0;
}

static int ctx_output(lua_State *L) { return ctxWrite(L, false); }
static int ctx_error(lua_State *L)  { return ctxWrite(L, true); }

static int ctx_is_redirected(lua_State *L)  // ctx:is_redirected("input"|"output"|"error"|"same")
{
    State *st;
    Command *cmd = commandOf(L, &st);
    const char *k = luaL_checkstring(L, 2);
    static const char *const kinds[] = {"input", "output", "error", "same", NULL};
    int which = -1;
    for (int i = 0; kinds[i]; i++) if (strcmpNoCase(k, kinds[i]) == 0) which = i;
    if (which < 0) return luaL_argerror(L, 2, "\"input\", \"output\", \"error\" or \"same\" expected");
    RexxIORedirectorContext *io = cmd->io;
    bool r = io != NULL && (which == 0 ? io->IsInputRedirected() : which == 1 ? io->IsOutputRedirected()
                            : which == 2 ? io->IsErrorRedirected() : io->AreOutputAndErrorSameTarget());
    lua_pushboolean(L, r);
    return 1;
}

// The variable names: Rexx's, so uppercase (pushes it)
static const char *ctxName(lua_State *L)
{
    luaL_checkstring(L, 2);
    pushUpper(L, 2, "");
    return lua_tostring(L, -1);
}

static int ctx_get_var(lua_State *L)        // ctx:get_var(name): its value, nil if it has none
{
    State *st;
    Command *cmd = commandOf(L, &st);
    const char *name = ctxName(L);
    lua_State *outerL = st->L;
    st->L = L;
    RexxObjectPtr v = cmd->c->GetContextVariable(name);
    if (st->tc->CheckCondition()) CTX_CHECK(L, st, outerL);
    if (v == NULLOBJECT) { st->L = outerL; lua_pushnil(L); return 1; }
    bool ok = false;
    if (!guarded(st, [&] { ok = pushValue(st, v); })) { st->L = outerL; return unwindError(L); }
    release(st->tc, v);                     // a userdata keeps it, if it is one
    st->L = outerL;
    if (!ok) return lua_error(L);           // its message
    return 1;
}

static int ctx_set_var(lua_State *L)        // ctx:set_var(name, value)
{
    State *st;
    Command *cmd = commandOf(L, &st);
    luaL_checkany(L, 3);
    const char *name = ctxName(L);
    lua_State *outerL = st->L;
    st->L = L;
    RexxObjectPtr v = toRexx(st, 3);
    if (!st->tc->CheckCondition()) cmd->c->SetContextVariable(name, v);
    release(st->tc, v);
    CTX_CHECK(L, st, outerL);
    return 0;
}

static int ctx_drop_var(lua_State *L)       // ctx:drop_var(name)
{
    State *st;
    Command *cmd = commandOf(L, &st);
    const char *name = ctxName(L);
    lua_State *outerL = st->L;
    st->L = L;
    cmd->c->DropContextVariable(name);
    CTX_CHECK(L, st, outerL);
    return 0;
}

// ctx:raise_error([rc]), ctx:raise_failure([rc]): ERROR (FAILURE) after the
// command, with RC the handler's value or, if it gives none, rc.  Returns rc.
static int ctxRaise(lua_State *L, const char *condition)
{
    State *st;
    Command *cmd = commandOf(L, &st);
    lua_settop(L, 2);
    cmd->condition = condition;
    cmd->rc = NULLOBJECT;
    if (!lua_isnil(L, 2)) {
        lua_State *outerL = st->L;
        st->L = L;
        cmd->rc = toRexx(st, 2);
        CTX_CHECK(L, st, outerL);
    }
    return 1;
}

static int ctx_raise_error(lua_State *L)   { return ctxRaise(L, "ERROR"); }
static int ctx_raise_failure(lua_State *L) { return ctxRaise(L, "FAILURE"); }

static int ctx_tostring(lua_State *L)
{
    lua_pushliteral(L, "ADDRESS ");
    lua_pushliteral(L, "environment");
    lua_rawget(L, 1);
    lua_pushliteral(L, " context");
    lua_concat(L, 3);
    return 1;
}

/* cmd_call(handler, command, id, environment): handler(command, ctx), its
 * first result.  The ctx is made here, protected: it allocates.            */
static int cmd_call(lua_State *L)
{
    lua_settop(L, 4);
    lua_createtable(L, 0, 1);                               // 5: ctx
    lua_pushliteral(L, "environment");
    lua_pushvalue(L, 4);
    lua_rawset(L, 5);
    lua_pushvalue(L, 3);
    lua_rawsetp(L, 5, &CTXID);
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &CTXMETA) != LUA_TTABLE) {
        lua_pop(L, 1);
        static const luaL_Reg methods[] = {
            {"line_in", ctx_line_in}, {"lines", ctx_lines}, {"output", ctx_output}, {"error", ctx_error},
            {"is_redirected", ctx_is_redirected}, {"get_var", ctx_get_var}, {"set_var", ctx_set_var},
            {"drop_var", ctx_drop_var}, {"raise_error", ctx_raise_error}, {"raise_failure", ctx_raise_failure},
            {NULL, NULL}};
        lua_createtable(L, 0, 4);
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, ctx_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushliteral(L, "ADDRESS context");
        lua_setfield(L, -2, "__name");
        lua_pushliteral(L, "ADDRESS context");
        lua_setfield(L, -2, "__metatable");                 // protected
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &CTXMETA);
    }
    lua_setmetatable(L, 5);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 5);
    lua_call(L, 2, 1);
    return 1;
}

/* The command, in the handler's state: RC its value (nil or none: 0), or
 * SYNTAX for a Lua error in it (98.900, as any Lua error reaching Rexx;
 * a Rexx condition that went through Lua, itself; a HALT, 4.1), and ERROR
 * or FAILURE after it if it asked for them (ctx:raise_error...).          */
static NOINLINE RexxObjectPtr runHandler(RexxExitContext *c, Proxy *px, RexxObjectPtr handler,
                                         RexxStringObject address, RexxStringObject command,
                                         RexxIORedirectorContext *io)
{
    RexxThreadContext *tc = c->threadContext;
    State *st = px->st;
    if (foreignThread(st)) { raise(tc, std::string("Lua error: ") + noRexx(st)); return NULLOBJECT; }
    RexxObjectPtr stateObj = tc->SendMessage0(handler, "LUA.STATE");
    if (stateObj == NULLOBJECT) return NULLOBJECT;
    Enter e(st, tc, stateObj);
    if (st->closed) { raise(tc, "Lua error: the LuaState is closed"); return NULLOBJECT; }
    releasePending(st);
    lua_State *L = st->L;
    int top = lua_gettop(L);
    if (!lua_checkstack(L, 8)) { raise(tc, "Lua error: Lua stack overflow"); return NULLOBJECT; }
    Command cmd;
    cmd.c = c;
    cmd.io = io;
    cmd.id = ++st->commands;
    cmd.prev = st->command;
    st->command = &cmd;
    // off the list however it ends (a SIGNAL past the bridge: pcallAll throws)
    struct Pop { State *st; Command *cmd; ~Pop() { if (st->command == cmd) st->command = cmd->prev; } } pop{st, &cmd};
    lua_pushcfunction(L, cmd_call);
    pushSelf(px);
    lua_pushlstring(L, c->StringData(command), c->StringLength(command));
    lua_pushinteger(L, cmd.id);
    lua_pushlstring(L, c->StringData(address), c->StringLength(address));
    int n = pcallAll(st, 4);
    st->command = cmd.prev;
    (void)pop;
    if (n < 0) {                            // raised; or Ctrl-C: the HALT after the command
        lua_settop(L, top);
        return tc->CheckCondition() ? NULLOBJECT : c->WholeNumber(1);
    }
    RexxObjectPtr rc = lua_isnil(L, -1) ? (cmd.rc != NULLOBJECT ? cmd.rc : c->WholeNumber(0)) : toRexx(st, -1);
    lua_settop(L, top);
    if (tc->CheckCondition()) return NULLOBJECT;            // making a .LuaObject failed
    if (cmd.condition != NULL) c->RaiseCondition(cmd.condition, command, NULLOBJECT, rc);
    return rc;
}

// The native handler of every environment written in Lua
static RexxObjectPtr RexxEntry luaCommand(RexxExitContext *c, RexxStringObject address,
                                           RexxStringObject command, RexxIORedirectorContext *io)
{
    RexxThreadContext *tc = c->threadContext;
    drain(tc);
    std::string name = upperString(c->StringData(address), c->StringLength(address));
    RexxObjectPtr t = commandsOf(tc, false);
    RexxObjectPtr handler = t == NULLOBJECT ? NULLOBJECT : tc->SendMessage1(t, "AT", tc->String(name.c_str()));
    Proxy *px = handler == NULLOBJECT || handler == tc->Nil() ? NULL : proxyOf(tc, handler);
    if (px == NULL) {                       // removed: as an unknown environment
        tc->ClearCondition();
        RexxObjectPtr rc = c->WholeNumber(RXSUBCOM_NOTREG);
        c->RaiseCondition("FAILURE", command, NULLOBJECT, rc);
        return rc;
    }
    return runHandler(c, px, handler, address, command, io);
}

// An interpreter instance the module made.  Never freed: Lua functions and
// states may still point to it after it is closed (they then see `closed`).
struct Inst {
    RexxInstance *rexx = NULL;
    State *host = NULL;                     // whose .lua it is
    RexxObjectPtr stateObj = NULLOBJECT;    // its .lua (a global reference)
    int depth = 0;                          // calls into it in progress
    bool closed = false;
    // its REDIRECTING_ENVIRONMENTS (LUA and command_environments' names),
    // kept: the option points to them
    std::vector<std::string> envNames;
    std::vector<RexxRedirectingEnvironment> envs;
};
static std::mutex instLock;
static std::map<RexxInstance *, Inst *> instances;     // the live ones

static Inst *instOfContext(RexxThreadContext *c)
{
    std::lock_guard<std::mutex> guard(instLock);
    auto it = instances.find(c->instance);
    return it == instances.end() ? NULL : it->second;
}

// The instance that calls from the host's Lua into Rexx objects attach to:
// any live instance of this host
static Inst *liveInst(State *st)
{
    if (st->inst != NULL && !st->inst->closed) return st->inst;
    st->inst = NULL;
    std::lock_guard<std::mutex> guard(instLock);
    for (auto &e : instances)
        if (e.second->host == st) { st->inst = e.second; break; }
    return st->inst;
}

// The host's thread, outside any Rexx code (st->tc NULL), calls into Rexx:
// see FROM_HOST.  Attaches to `in` (NULL: stays in the Rexx code running,
// st->tc), runs the C function now running again, in lua_pcall, with GUARD
// as its first argument, and puts everything back before raising its error.
// No C++ object here (a Lua error is raised in this frame).
static int hostRun(lua_State *L, State *st, Inst *in, lua_CFunction self, int nup)
{
    lua_State *savedL = st->L;
    RexxThreadContext *savedTc = st->tc;
    RexxObjectPtr savedSelf = st->self;
    int savedDepth = st->depth;
    RexxThreadContext *tc = NULL;
    if (in != NULL) {
        if (in->closed) return luaL_error(L, "the Rexx interpreter instance is closed");
        if (!in->rexx->AttachThread(&tc)) return luaL_error(L, "cannot attach to the Rexx interpreter instance");
        st->tc = tc;
        st->self = in->stateObj;
        st->depth++;
        in->depth++;
        drain(tc);
        releasePending(st);
    }
    else if (st->tc == NULL) return luaL_error(L, "no Rexx code is running");
    int n = lua_gettop(L);
    if (!lua_checkstack(L, nup + 3)) return luaL_error(L, "Lua stack overflow");
    for (int i = 1; i <= nup; i++) lua_pushvalue(L, lua_upvalueindex(i));
    lua_pushcclosure(L, self, nup);                 // this very C function, again
    lua_insert(L, 1);
    lua_pushlightuserdata(L, &GUARD);
    lua_insert(L, 2);
    int rc = lua_pcall(L, n + 1, LUA_MULTRET, 0);
    st->L = savedL;
    st->tc = savedTc;
    st->self = savedSelf;
    st->depth = savedDepth;
    if (in != NULL) { in->depth--; tc->DetachThread(); }
    if (rc != LUA_OK) return lua_error(L);
    return lua_gettop(L);
}

// A Rexx object or routine called by the host's Lua outside any Rexx code:
// attached to the instance that made it (hint) or, if that one is closed,
// to any live one of this host
static int fromHost(lua_State *L, Inst *hint, lua_CFunction self, int nup)
{
    State *st = stateOf(L);
    if (!st->adopted || st->closed || foreignThread(st)) return luaL_error(L, "%s", noRexx(st));
    Inst *in = hint != NULL && !hint->closed ? hint : liveInst(st);
    if (in == NULL) return luaL_error(L, "%s", noRexx(st));
    return hostRun(L, st, in, self, nup);
}

static const char *noRexx(State *st)
{
    if (foreignThread(st)) return "the Lua state that runs ooRexx can only be used from its own thread";
    if (st->closed) return "the Lua state is closed";
    return "no Rexx interpreter instance is open";
}

// Terminate an instance (on the host's thread, which made it)
static NOINLINE void closeInst(Inst *in)
{
    if (in->closed) return;
    in->closed = true;
    {
        std::lock_guard<std::mutex> guard(instLock);
        instances.erase(in->rexx);
    }
    if (in->host->inst == in) in->host->inst = NULL;
    // the creating thread stayed attached: detaching it makes Terminate wait
    // forever (ooRexx bug #2107)
    in->rexx->Terminate();
}

// The directory this library is in (lua.cls is next to it)
static std::string moduleDirectory()
{
#ifdef _WIN32
    HMODULE self;
    char buf[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&moduleDirectory, &self) ||
        GetModuleFileNameA(self, buf, sizeof buf) == 0) return ".";
    std::string path = buf;
#else
    Dl_info info;
    if (dladdr((void *)&moduleDirectory, &info) == 0 || info.dli_fname == NULL) return ".";
    std::string path = info.dli_fname;
#endif
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

static std::string join(const std::vector<std::string> &v, const char *sep)
{
    std::string r;
    for (const std::string &x : v) { if (!r.empty()) r += sep; r += x; }
    return r;
}

static std::string conditionText(RexxThreadContext *tc)
{
    RexxCondition cd;
    tc->DecodeConditionInfo(tc->GetConditionInfo(), &cd);
    tc->ClearCondition();
    std::string m = "Rexx error " + std::to_string(cd.code / 1000) + "." + std::to_string(cd.code % 1000);
    if (cd.message) { m += ": "; m.append(tc->StringData(cd.message), tc->StringLength(cd.message)); }
    else if (cd.errortext) { m += ": "; m.append(tc->StringData(cd.errortext), tc->StringLength(cd.errortext)); }
    return m;
}

struct Options {
    std::vector<std::string> path, extensions, libraries;
    std::vector<std::string> environments;  // command_environments' names, uppercase
    std::string address;
};

// A new instance for the host's state st, or NULL and the reason
static Inst *newInst(State *st, const Options &o, std::string &error)
{
    std::string dir = moduleDirectory();
    std::vector<std::string> path = o.path;
    path.push_back(dir);
#ifdef _WIN32
    std::string pathText = join(path, ";");
#else
    std::string pathText = join(path, ":");
#endif
    std::string extText = join(o.extensions, ",");
    static RexxLibraryPackage library[] = {{"rexxlua", &rexxlua_package_entry}, {NULL, NULL}};
    std::vector<RexxOption> options;
    RexxOption opt;
    opt.optionName = EXTERNAL_CALL_PATH; opt.option = pathText.c_str(); options.push_back(opt);
    if (!o.extensions.empty()) { opt.optionName = EXTERNAL_CALL_EXTENSIONS; opt.option = extText.c_str(); options.push_back(opt); }
    if (!o.address.empty()) { opt.optionName = INITIAL_ADDRESS_ENVIRONMENT; opt.option = o.address.c_str(); options.push_back(opt); }
    for (const std::string &l : o.libraries) { opt.optionName = LOAD_REQUIRED_LIBRARY; opt.option = l.c_str(); options.push_back(opt); }
    opt.optionName = REGISTER_LIBRARY; opt.option = (POINTER)library; options.push_back(opt);
    // ADDRESS LUA, and the names of command_environments, from the start (not
    // AddCommandEnvironment: docs/design.md, *ADDRESS LUA*, *Command environments*)
    Inst *in = new Inst();
    in->envNames = o.environments;
    in->envs.push_back({addressLua, "LUA"});
    for (const std::string &n : in->envNames) in->envs.push_back({luaCommand, n.c_str()});
    in->envs.push_back({NULL, NULL});
    opt.optionName = REDIRECTING_ENVIRONMENTS; opt.option = (POINTER)in->envs.data(); options.push_back(opt);
    opt.optionName = NULL; options.push_back(opt);

    RexxInstance *rexx;
    RexxThreadContext *tc;
    if (!RexxCreateInterpreter(&rexx, &tc, options.data())) {
        error = "cannot create a Rexx interpreter instance";
        delete in;
        return NULL;
    }
    in->rexx = rexx;
    in->host = st;
    {
        std::lock_guard<std::mutex> guard(instLock);
        instances[rexx] = in;               // before lua.cls asks for its host (Lua_AdoptNext)
    }
    if (st->keep == NULLOBJECT) {
        RexxObjectPtr keep = tc->SendMessage0(tc->FindClass("IDENTITYTABLE"), "NEW");
        st->keep = tc->RequestGlobalReference(keep);  // never released (#2100)
    }
    std::string cls = dir + "/lua.cls";
    RexxPackageObject package = tc->LoadPackage(cls.c_str());
    if (package == NULLOBJECT || tc->CheckCondition()) {
        error = tc->CheckCondition() ? conditionText(tc) : "cannot load " + cls;
        closeInst(in);
        return NULL;
    }
    // .lua: lua.cls sets it when it is loaded; a package already loaded (by
    // another live instance) is not run again, so for a second instance
    // it is set here
    RexxDirectoryObject local = tc->GetLocalEnvironment();
    RexxObjectPtr lua = tc->DirectoryAt(local, "LUA");
    if (lua == NULLOBJECT) {
        lua = tc->SendMessage0(tc->FindPackageClass(package, "LUASTATE"), "LUA.DEFAULT");
        if (lua != NULLOBJECT) tc->DirectoryPut(local, lua, "LUA");
    }
    if (lua == NULLOBJECT || tc->CheckCondition()) {
        error = tc->CheckCondition() ? conditionText(tc) : "cannot make .lua";
        closeInst(in);
        return NULL;
    }
    in->stateObj = tc->RequestGlobalReference(lua);
    // the names ooRexx has already: in the instance's names, with no handler
    // yet (mod_interpreter adds them), so never given to ooRexx again
    if (!in->envNames.empty()) {
        RexxObjectPtr t = commandsOf(tc, true);
        for (const std::string &n : in->envNames)
            if (t != NULLOBJECT) tc->SendMessage2(t, "PUT", tc->Nil(), tc->String(n.c_str()));
        if (t == NULLOBJECT || tc->CheckCondition()) {
            error = tc->CheckCondition() ? conditionText(tc) : "cannot make the instance's command environments";
            closeInst(in);
            return NULL;
        }
    }
    if (st->inst == NULL) st->inst = in;
    return in;
}

typedef std::function<RexxObjectPtr(RexxThreadContext *, RexxArrayObject)> RexxOp;

// A routine from source, which sees lua.cls's public classes (.LuaState,
// .LuaObject) as if it required it
static NOINLINE RexxRoutineObject newRoutine(RexxThreadContext *tc, const char *name, const std::string &source)
{
    RexxRoutineObject r = tc->NewRoutine(name, source.data(), source.size());
    if (r == NULLOBJECT || luaObjectClass == NULLOBJECT) return r;
    RexxObjectPtr lua = tc->SendMessage0(luaObjectClass, "PACKAGE");
    RexxPackageObject p = tc->GetRoutinePackage(r);
    if (lua != NULLOBJECT && p != NULLOBJECT) release(tc, tc->SendMessage1(p, "ADDPACKAGE", lua));
    release(tc, lua);
    release(tc, p);
    return r;
}

/*
 * Run op in Rexx for Lua code in L, with the Lua arguments from `first` on as
 * its arguments, on the thread context of the Rexx code running (hostRun
 * attached it, or a native method of a state the bridge made is running).
 * Pushes the result (nothing for none) and returns how many values it
 * pushed, or pushes a message and returns -1: the caller raises it, once the
 * C++ objects here are gone.  What it makes is released (a loop of calls
 * inside one Rexx call must not grow: *Lifetime*, local references).
 */
static NOINLINE int intoRexx(lua_State *L, int first, const RexxOp &op)
{
    State *st = stateOf(L);
    RexxThreadContext *tc = st->tc;
    if (tc == NULL) { lua_pushstring(L, "no Rexx code is running"); return -1; }
    if (!lua_checkstack(L, 8)) { lua_pushstring(L, "Lua stack overflow"); return -1; }
    st->L = L;                                      // (hostRun puts it back)
    int top = lua_gettop(L);
    RexxArrayObject args = tc->NewArray(top >= first ? top - first + 1 : 0);
    for (int i = first; i <= top; i++) {
        RexxObjectPtr v = toRexx(st, i);
        if (v != tc->Nil()) tc->ArrayPut(args, v, i - first + 1);
        release(tc, v);                             // the Array holds it
    }
    lua_settop(L, top);
    RexxObjectPtr r = NULLOBJECT;
    // a SIGNAL past the bridge (guarded): its Lua error, no more Rexx calls
    if (!guarded(st, [&] { if (!tc->CheckCondition()) r = op(tc, args); })) {
        lua_pushliteral(L, "a Rexx SIGNAL is unwinding through Lua");
        return -1;
    }
    release(tc, args);
    int n;
    if (tc->CheckCondition()) { pushRexxCondition(L, tc); n = -1; }
    else if (r == NULLOBJECT) n = 0;
    else {
        bool ok = false;
        if (!guarded(st, [&] { ok = pushValue(st, r); })) {
            lua_pushliteral(L, "a Rexx SIGNAL is unwinding through Lua");
            return -1;
        }
        n = ok ? 1 : -1;                            // its message, if it fails
    }
    release(tc, r);                                 // a userdata keeps it, if it is one
    return n;
}

// The instance a module function works on.  Upvalue 1: an Inst (light
// userdata); false for the host's default instance, made at first use; nil
// for the one whose Rexx code is running.  False after pushing a message.
static NOINLINE bool instFor(lua_State *L, Inst **out)
{
    int t = lua_type(L, lua_upvalueindex(1));
    if (t == LUA_TLIGHTUSERDATA) { *out = (Inst *)lua_touserdata(L, lua_upvalueindex(1)); return true; }
    *out = NULL;
    if (t == LUA_TNIL) return true;
    State *st = stateOf(L);
    if (st->defaultInst == NULL) {
        std::string error;
        st->defaultInst = newInst(st, Options(), error);
        if (st->defaultInst == NULL) { lua_pushlstring(L, error.data(), error.size()); return false; }
    }
    *out = st->defaultInst;
    return true;
}

// A module function, called: its instance, and then the function again,
// protected (hostRun)
static int moduleCall(lua_State *L, lua_CFunction self)
{
    State *st = stateOf(L);
    if (st->closed || foreignThread(st)) return luaL_error(L, "%s", noRexx(st));
    Inst *in;
    if (!instFor(L, &in)) return lua_error(L);
    return hostRun(L, st, in, self, 1);
}
#define MODULE_ENTRY(L, self)                                                 \
    if (lua_touserdata(L, 1) == &GUARD) lua_remove(L, 1);                     \
    else return moduleCall(L, self)

/*
 * The module's functions raise their Lua errors (lua_error) only once every
 * C++ object is gone: those objects live in the NOINLINE *_body functions,
 * never in the frame that raises (see NOINLINE).
 */

// rexx.eval(expression, args...): its value
static NOINLINE int eval_body(lua_State *L, const char *expr, size_t len)
{
    std::string source = std::string("return ") + std::string(expr, len);
    return intoRexx(L, 2, [&](RexxThreadContext *tc, RexxArrayObject args) -> RexxObjectPtr {
        RexxRoutineObject r = newRoutine(tc, "EVAL", source);
        if (r == NULLOBJECT) return NULLOBJECT;
        RexxObjectPtr result = tc->CallRoutine(r, args);
        release(tc, r);
        return result;
    });
}

static int mod_eval(lua_State *L)
{
    MODULE_ENTRY(L, mod_eval);
    size_t len;
    const char *expr = luaL_checklstring(L, 1, &len);
    int n = eval_body(L, expr, len);
    if (n < 0) return lua_error(L);
    return n;
}

// rexx.run(program, args...): CALL program, its result
static NOINLINE int run_body(lua_State *L, const char *program)
{
    return intoRexx(L, 2, [&](RexxThreadContext *tc, RexxArrayObject args) -> RexxObjectPtr {
        return tc->CallProgram(program, args);
    });
}

static int mod_run(lua_State *L)
{
    MODULE_ENTRY(L, mod_run);
    const char *program = luaL_checkstring(L, 1);   // (Lua keeps it: argument 1)
    int n = run_body(L, program);
    if (n < 0) return lua_error(L);
    return n;
}

// rexx.routine(source[, name]): a Rexx Routine, as a Lua function
static NOINLINE int routine_body(lua_State *L, const char *source, size_t len, const char *name)
{
    std::string src(source, len);
    return intoRexx(L, 3, [&](RexxThreadContext *tc, RexxArrayObject) -> RexxObjectPtr {
        return (RexxObjectPtr)newRoutine(tc, name, src);
    });
}

static int mod_routine(lua_State *L)
{
    MODULE_ENTRY(L, mod_routine);
    size_t len;
    const char *source = luaL_checklstring(L, 1, &len);
    const char *name = luaL_optstring(L, 2, "ROUTINE");
    lua_settop(L, 2);
    int n = routine_body(L, source, len, name);
    if (n < 0) return lua_error(L);
    return n;
}

// r.add_command_environment(name, handler), r.remove_command_environment(name):
// ADDRESS name in that instance (docs/design.md, *Command environments*).
// Checked here first, so that a wrong argument is a plain Lua error.
static int checkEnvironmentName(lua_State *L, int idx)
{
    size_t n;
    const char *s = luaL_checklstring(L, idx, &n);
    if (n == 0) return luaL_argerror(L, idx, "the name of the environment is empty");
    if (strcmpNoCase(s, "LUA") == 0) return luaL_argerror(L, idx, "LUA is the bridge's own environment (ADDRESS LUA)");
    return 0;
}

static NOINLINE int addenv_body(lua_State *L)
{
    return intoRexx(L, 1, [&](RexxThreadContext *tc, RexxArrayObject args) -> RexxObjectPtr {
        addEnvironment(tc, tc->ArrayAt(args, 1), tc->ArrayAt(args, 2), "add_command_environment");
        return NULLOBJECT;
    });
}

static int mod_add_command_environment(lua_State *L)
{
    MODULE_ENTRY(L, mod_add_command_environment);
    lua_settop(L, 2);
    checkEnvironmentName(L, 1);
    if (!isCallable(L, 2))
        return luaL_argerror(L, 2, "a function, or a value with __call, expected (to remove an environment: "
                                   "remove_command_environment)");
    int n = addenv_body(L);
    if (n < 0) return lua_error(L);
    return 0;
}

static NOINLINE int removeenv_body(lua_State *L, bool *missing)
{
    return intoRexx(L, 1, [&](RexxThreadContext *tc, RexxArrayObject args) -> RexxObjectPtr {
        removeEnvironment(tc, tc->ArrayAt(args, 1), "remove_command_environment", missing);
        return NULLOBJECT;
    });
}

static int mod_remove_command_environment(lua_State *L)
{
    MODULE_ENTRY(L, mod_remove_command_environment);
    lua_settop(L, 1);
    checkEnvironmentName(L, 1);
    bool missing = false;
    int n = removeenv_body(L, &missing);
    if (n < 0) return lua_error(L);
    if (missing) return luaL_error(L, "remove_command_environment: %s has no handler", lua_tostring(L, 1));
    return 0;
}

// r.close(): Terminate the instance
static int mod_close(lua_State *L)
{
    Inst *in = (Inst *)lua_touserdata(L, lua_upvalueindex(1));
    if (in->closed) return 0;
    if (foreignThread(in->host)) return luaL_error(L, "%s", noRexx(in->host));
    if (in->depth > 0) return luaL_error(L, "a Rexx interpreter instance cannot be closed while its Rexx code is running");
    closeInst(in);
    return 0;
}

static void pushModuleFunctions(lua_State *L)        // upvalue on top: popped
{
    static const luaL_Reg fns[] = {{"eval", mod_eval}, {"run", mod_run}, {"routine", mod_routine},
                                   {"add_command_environment", mod_add_command_environment},
                                   {"remove_command_environment", mod_remove_command_environment}, {NULL, NULL}};
    luaL_setfuncs(L, fns, 1);
}

// An option that is a string or a sequence of strings (checked: no error later)
// (Options are read raw: an __index of the table cannot run, and raise an
// error, once there are C++ objects.)
static void checkStrings(lua_State *L, int opts, const char *key)
{
    lua_pushstring(L, key);
    int t = lua_rawget(L, opts);
    if (t == LUA_TTABLE) {
        for (lua_Integer i = 1; ; i++) {
            int u = lua_rawgeti(L, -1, i);
            lua_pop(L, 1);
            if (u == LUA_TNIL) break;
            if (u != LUA_TSTRING) luaL_error(L, "interpreter: %s[%d] must be a string", key, (int)i);
        }
    }
    else if (t != LUA_TNIL && t != LUA_TSTRING) luaL_error(L, "interpreter: %s must be a string or a list of strings", key);
    lua_pop(L, 1);
}

static void readStrings(lua_State *L, int opts, const char *key, std::vector<std::string> &out)
{
    lua_pushstring(L, key);
    if (lua_rawget(L, opts) == LUA_TTABLE) {
        for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TSTRING; i++) {
            out.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    else if (lua_type(L, -1) == LUA_TSTRING) out.push_back(lua_tostring(L, -1));
    lua_pop(L, 1);
}

// The options (checked) at 1 -> a new instance, or NULL after pushing why
static NOINLINE Inst *interpreter_body(lua_State *L, State *st)
{
    Options o;
    readStrings(L, 1, "rexx_path", o.path);
    readStrings(L, 1, "extensions", o.extensions);
    readStrings(L, 1, "load_libraries", o.libraries);
    lua_pushstring(L, "initial_address");
    if (lua_rawget(L, 1) == LUA_TSTRING) o.address = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_pushstring(L, "command_environments");
    if (lua_rawget(L, 1) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lua_pop(L, 1);
            size_t n;
            const char *k = lua_tolstring(L, -1, &n);       // (a string: checked)
            o.environments.push_back(upperString(k, n));
        }
    }
    lua_pop(L, 1);
    std::string error;
    Inst *in = newInst(st, o, error);
    if (in == NULL) lua_pushlstring(L, error.data(), error.size());
    return in;
}

// command_environments: a table, name -> callable; names not empty, not LUA,
// not twice (in any case)
static void checkEnvironments(lua_State *L, int opts)
{
    lua_pushstring(L, "command_environments");
    int t = lua_rawget(L, opts);
    if (t == LUA_TNIL) { lua_pop(L, 1); return; }
    if (t != LUA_TTABLE) luaL_error(L, "interpreter: command_environments must be a table, name -> handler");
    int envs = lua_gettop(L);
    lua_newtable(L);                                        // the names seen, uppercase
    int seen = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, envs)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "interpreter: command_environments: a name must be a string");
        const char *name = lua_tostring(L, -2);
        if (*name == '\0') luaL_error(L, "interpreter: command_environments: a name is empty");
        if (strcmpNoCase(name, "LUA") == 0)
            luaL_error(L, "interpreter: command_environments: LUA is the bridge's own environment (ADDRESS LUA)");
        if (!isCallable(L, -1))
            luaL_error(L, "interpreter: command_environments: the handler of %s must be a function, or a value with __call", name);
        lua_pop(L, 1);
        pushUpper(L, -1, "");
        lua_pushvalue(L, -1);
        if (lua_rawget(L, seen) != LUA_TNIL)
            luaL_error(L, "interpreter: command_environments: %s is there twice (names are uppercase in Rexx)", lua_tostring(L, -2));
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
        lua_rawset(L, seen);
    }
    lua_settop(L, envs - 1);
}

// rexx.interpreter{rexx_path=, extensions=, initial_address=, load_libraries=, command_environments=}
static int mod_interpreter(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) { lua_settop(L, 0); lua_newtable(L); }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    // checked first, so that no Lua error happens once there are C++ objects
    static const char *known[] = {"rexx_path", "extensions", "initial_address", "load_libraries",
                                  "command_environments", NULL};
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        lua_pop(L, 1);
        bool ok = false;
        if (lua_type(L, -1) == LUA_TSTRING)
            for (int i = 0; known[i]; i++) if (strcmp(lua_tostring(L, -1), known[i]) == 0) ok = true;
        if (!ok) return luaL_error(L, "interpreter: unknown option %s (rexx_path, extensions, "
                                      "initial_address, load_libraries, command_environments)", luaL_tolstring(L, -1, NULL));
    }
    checkStrings(L, 1, "rexx_path");
    checkStrings(L, 1, "extensions");
    checkStrings(L, 1, "load_libraries");
    lua_pushstring(L, "initial_address");
    int t = lua_rawget(L, 1);
    if (t != LUA_TNIL && t != LUA_TSTRING) return luaL_error(L, "interpreter: initial_address must be a string");
    lua_pop(L, 1);
    checkEnvironments(L, 1);
    State *st = stateOf(L);
    if (st->closed) return luaL_error(L, "the Lua state is closed");
    if (foreignThread(st)) return luaL_error(L, "%s", noRexx(st));
    Inst *in = interpreter_body(L, st);
    if (in == NULL) return lua_error(L);
    lua_newtable(L);
    lua_pushlightuserdata(L, in);
    pushModuleFunctions(L);
    lua_pushlightuserdata(L, in);
    lua_pushcclosure(L, mod_close, 1);
    lua_setfield(L, -2, "close");
    // the handlers of command_environments (their names ooRexx has already)
    lua_pushstring(L, "command_environments");
    if (lua_rawget(L, 1) == LUA_TTABLE) {
        int envs = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, envs)) {
            lua_getfield(L, envs - 1, "add_command_environment");
            lua_pushvalue(L, -3);
            lua_pushvalue(L, -3);
            lua_call(L, 2, 0);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 1;
}

// The host's state, closing (the __gc of a userdata in its registry, made
// when it was adopted, so finalized after every Rexx object's userdata):
// its instances are terminated, and Rexx's proxies of its Lua values dead
struct HostSentinel { State *st; };
static char SENTINELKEY;

static int host_gc(lua_State *L)
{
    State *st = ((HostSentinel *)lua_touserdata(L, 1))->st;
    if (st->closed) return 0;
    st->closed = true;
    st->pending.clear();
    std::vector<Inst *> mine;
    {
        std::lock_guard<std::mutex> guard(instLock);
        for (auto &e : instances) if (e.second->host == st) mine.push_back(e.second);
    }
    RexxThreadContext *tc;
    if (!mine.empty() && st->keep != NULLOBJECT && mine[0]->rexx->AttachThread(&tc)) {
        tc->SendMessage0(st->keep, "EMPTY");
        tc->DetachThread();
    }
    for (Inst *in : mine) closeInst(in);
    st->L = st->main = NULL;
    st->inst = NULL;
    return 0;
}

MODULE_EXPORT int luaopen_oorexx(lua_State *L)
{
    luaL_checkversion(L);
    State *st = stateOf(L);
    if (st == NULL) {                       // a host's Lua: adopt its state
        st = new State();
        st->adopted = true;
        st->owner = std::this_thread::get_id();
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
        st->L = st->main = lua_tothread(L, -1);
        lua_pop(L, 1);
        setupLua(L, st);
        HostSentinel *h = (HostSentinel *)lua_newuserdatauv(L, sizeof(HostSentinel), 0);
        h->st = st;
        lua_newtable(L);
        lua_pushcfunction(L, host_gc);
        lua_setfield(L, -2, "__gc");
        lua_setmetatable(L, -2);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &SENTINELKEY);
    }
    else if (st->closed) return luaL_error(L, "the Lua state is closed");
    lua_newtable(L);
    if (st->adopted) lua_pushboolean(L, 0);     // the default instance
    else lua_pushnil(L);                        // the one whose Rexx code is running
    pushModuleFunctions(L);
    if (st->adopted) {
        lua_pushcfunction(L, mod_interpreter);
        lua_setfield(L, -2, "interpreter");
    }
    return 1;
}

// .LuaState~"LUA.ADOPTNEXT" (lua.cls, LUA.DEFAULT): if the module made the
// current instance, the next .LuaState~new on this thread is its host's
// state (LuaState_New), and 1; else 0.  (Not an argument of init, which
// anyone can give.)
RexxMethod0(logical_t, Lua_AdoptNext)
{
    Inst *in = instOfContext(context->threadContext);
    if (in == NULL || in->host == NULL || in->host->closed) return 0;
    adoptNext = in->host;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* ADDRESS LUA ... WITH (docs/design.md, *ADDRESS LUA*, *WITH*)               */
/* ------------------------------------------------------------------------ */

/*
 * While a command with redirection runs, Lua functions of the bridge stand
 * in for print, io.stdout (OUTPUT), io.stderr (ERROR), io.write, io.output
 * (either), io.read, io.lines, io.input and io.stdin (INPUT), and io.type and
 * io.close (any), as what is redirected asks (rd_install); afterwards each
 * name gets its value back, unless the command changed it.  The stand-ins
 * carry the number of their command: called after it ended (kept in a
 * variable), they find no Redirect and raise an error.  Output goes through
 * the redirector's Write*Buffer, which cuts it into lines (and keeps a line
 * cut in two by two writes); input is read a line at a time, ReadInput.
 * Every one of them runs inside the command, so under the state's lock, in
 * its thread.
 */
struct Redirect {
    RexxIORedirectorContext *io = NULL;
    lua_Integer id = 0;
    bool in = false, out = false, err = false;     // what is redirected
    lua_State *L = NULL;                            // the Lua thread of the read in progress
    Redirect *prev = NULL;
    std::string buf;                                // input read, not yet consumed: buf[pos..]
    size_t pos = 0;
    bool eof = false;
};

static Redirect *redirectOf(lua_State *L, State **pst)
{
    State *st = stateOf(L);
    lua_Integer id = lua_tointeger(L, lua_upvalueindex(1));
    Redirect *r = NULL;
    if (st != NULL) for (r = st->redirect; r != NULL && r->id != id; r = r->prev) {}
    if (r == NULL) luaL_error(L, "the ADDRESS LUA command this stream was redirected for has ended");
    *pst = st;
    return r;
}

// Writes the arguments from first on, as io.write does (strings, numbers),
// to the command's output or error.  A Rexx condition the target raises is
// a Lua error.
static int rdWrite(lua_State *L, State *st, Redirect *r, int first, bool err)
{
    int top = lua_gettop(L);
    for (int i = first; i <= top; i++) {
        char num[64];
        size_t len;
        const char *s;
        if (lua_type(L, i) == LUA_TNUMBER) {        // as this Lua's io.write writes it
#if LUA_VERSION_NUM >= 505
            len = lua_numbertocstring(L, i, num);
            len = len > 0 ? len - 1 : 0;            // (its count has the NUL)
#else
            int k = lua_isinteger(L, i) ? snprintf(num, sizeof num, LUA_INTEGER_FMT, (lua_Integer)lua_tointeger(L, i))
                                        : snprintf(num, sizeof num, LUA_NUMBER_FMT, (lua_Number)lua_tonumber(L, i));
            len = k < 0 ? 0 : (size_t)k;
#endif
            s = num;
        }
        else s = luaL_checklstring(L, i, &len);
        lua_State *outerL = st->L;
        st->L = L;                          // a USING target may call back into Lua
        bool ran = guarded(st, [&] { if (err) r->io->WriteErrorBuffer(s, len); else r->io->WriteOutputBuffer(s, len); });
        st->L = outerL;
        if (!ran) return unwindError(L);
        if (st->tc->CheckCondition()) { pushRexxCondition(L, st->tc); return lua_error(L); }
    }
    return 0;
}

static int rd_print(lua_State *L)                   // print(...)
{
    State *st;
    Redirect *r = redirectOf(L, &st);
    int n = lua_gettop(L);
    luaL_Buffer b;
#if LUA_VERSION_NUM < 504
    lua_getglobal(L, "tostring");           // Lua 5.3's print calls the global tostring
    int ts = lua_gettop(L);
#endif
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');
#if LUA_VERSION_NUM < 504
        lua_pushvalue(L, ts);
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        if (lua_type(L, -1) != LUA_TSTRING && lua_type(L, -1) != LUA_TNUMBER)
            return luaL_error(L, "'tostring' must return a string to 'print'");
#else
        luaL_tolstring(L, i, NULL);
#endif
        luaL_addvalue(&b);
    }
    luaL_addchar(&b, '\n');
    luaL_pushresult(&b);                    // one write: the line is not cut
    rdWrite(L, st, r, lua_gettop(L), false);
    return 0;
}

// The functions of io that stand in (io.write, io.read, io.lines, io.output,
// io.input, io.type) have two upvalues: the command's number and a table,
// ctx, with Lua's own functions (by name: "write", "read"...), the real
// standard files ("rIn", "rOut", "rErr") and the stand-ins of those that are
// redirected ("sIn", "sOut", "sErr").  Lua's default files decide: where the
// default output is the real io.stdout (io.stderr) and that is redirected,
// io.write goes to the redirection; elsewhere, to Lua's io.write.  So
// io.output("log.txt") works as it does in Lua.

static void rdCtx(lua_State *L, const char *key)            // pushes ctx[key]
{
    lua_pushstring(L, key);
    lua_rawget(L, lua_upvalueindex(2));
}

// Lua's function name (in ctx), with the arguments on the stack: its
// results.  Its errors are Lua's, but called from here Lua cannot name it
// or say where it was called from: "bad argument #1 to '?'" gets both back.
static int rdDelegate(lua_State *L, const char *name)
{
    rdCtx(L, name);
    if (!lua_isfunction(L, -1)) return luaL_error(L, "io.%s is not available", name);
    lua_insert(L, 1);
    if (lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0) != LUA_OK) {
        const char *m = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
        if (m != NULL && strstr(m, "'?'") != NULL) {
            char fn[32];
            snprintf(fn, sizeof fn, "'%s'", name);
            luaL_where(L, 1);
            luaL_gsub(L, m, "'?'", fn);
            lua_concat(L, 2);
        }
        return lua_error(L);
    }
    return lua_gettop(L);
}

// Which stand-in Lua's default output (input) is now: 1 io.stdout's, 2
// io.stderr's, 3 io.stdin's, 0 none (a file of Lua's).  Without Lua's
// io.output (io.input), the stand-in of the redirected stream.
static int rdCurrent(lua_State *L, bool input)
{
    luaL_checkstack(L, 4, NULL);
    rdCtx(L, input ? "input" : "output");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        rdCtx(L, input ? "sIn" : "sOut");
        int k = lua_isnil(L, -1) ? 0 : input ? 3 : 1;
        lua_pop(L, 1);
        return k;
    }
    lua_call(L, 0, 1);
    static const char *const real[] = {"rOut", "rErr", "rIn"}, *const sub[] = {"sOut", "sErr", "sIn"};
    int k = 0;
    for (int i = input ? 2 : 0; i < (input ? 3 : 2) && k == 0; i++) {
        rdCtx(L, sub[i]);
        rdCtx(L, real[i]);
        if (!lua_isnil(L, -2) && !lua_isnil(L, -1) && lua_rawequal(L, -1, -3)) k = i + 1;
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    return k;
}

static int rd_iowrite(lua_State *L)                 // io.write(...): returns the file
{
    State *st;
    Redirect *r = redirectOf(L, &st);
    int k = rdCurrent(L, false);
    if (k == 0) return rdDelegate(L, "write");
    rdWrite(L, st, r, 1, k == 2);
    rdCtx(L, k == 2 ? "sErr" : "sOut");
    return 1;
}

// The value at idx: a stand-in of this command becomes the real file it
// stands for (toReal), or a real file whose stream is redirected becomes its
// stand-in (!toReal); anything else stays
static void rdMap(lua_State *L, int idx, bool toReal)
{
    static const char *const sub[] = {"sIn", "sOut", "sErr"}, *const real[] = {"rIn", "rOut", "rErr"};
    idx = lua_absindex(L, idx);
    luaL_checkstack(L, 3, NULL);
    for (int i = 0; i < 3; i++) {
        rdCtx(L, sub[i]);                           // not redirected: no stand-in
        rdCtx(L, real[i]);
        if (!lua_isnil(L, -2) && !lua_isnil(L, -1) && lua_rawequal(L, toReal ? -2 : -1, idx)) {
            lua_pushvalue(L, toReal ? -1 : -2);
            lua_replace(L, idx);
            lua_pop(L, 2);
            return;
        }
        lua_pop(L, 2);
    }
}

// io.output(file), io.input(file): Lua's, with the stand-ins as the real
// files they stand for, both ways
static int rdSetDefault(lua_State *L, const char *name)
{
    State *st;
    redirectOf(L, &st);
    if (!lua_isnoneornil(L, 1)) { lua_settop(L, 1); rdMap(L, 1, true); }
    else lua_settop(L, 0);
    rdDelegate(L, name);
    lua_settop(L, 1);
    rdMap(L, 1, false);
    return 1;
}

static int rd_iooutput(lua_State *L) { return rdSetDefault(L, "output"); }
static int rd_ioinput(lua_State *L) { return rdSetDefault(L, "input"); }

static int rd_ioclose(lua_State *L)                 // io.close(file): Lua's, with the real file
{
    if (!lua_isnoneornil(L, 1)) { lua_settop(L, 1); rdMap(L, 1, true); }
    return rdDelegate(L, "close");
}

static char rdMark;                                 // key, in a stand-in's metatable, of its command's number

static int rd_iotype(lua_State *L)                  // io.type(x): "file", "closed file" for a stand-in
{
    if (lua_istable(L, 1) && lua_getmetatable(L, 1)) {
        lua_rawgetp(L, -1, &rdMark);
        if (lua_isinteger(L, -1)) {
            lua_Integer id = lua_tointeger(L, -1);
            State *st = stateOf(L);
            Redirect *r = NULL;
            if (st != NULL) for (r = st->redirect; r != NULL && r->id != id; r = r->prev) {}
            lua_pushstring(L, r != NULL ? "file" : "closed file");
            return 1;
        }
        lua_pop(L, 2);
    }
    return rdDelegate(L, "type");
}

static int rd_fwrite(lua_State *L)                  // f:write(...): returns f
{
    State *st;
    Redirect *r = redirectOf(L, &st);
    rdWrite(L, st, r, 2, lua_toboolean(L, lua_upvalueindex(2)) != 0);
    lua_settop(L, 1);
    return 1;
}

// One more line of input into r->buf (with "\n": the redirector's lines have
// no line end); false at the end, or with a Rexx condition pending
static NOINLINE bool rdFill(State *st, Redirect *r)
{
    if (r->eof) return false;
    CSTRING data = NULL;
    size_t len = 0;
    lua_State *outerL = st->L;
    st->L = r->L;                           // a USING source may call back into Lua
    bool ran = guarded(st, [&] { r->io->ReadInput(&data, &len); });
    bool error = !ran || st->tc->CheckCondition();      // (an unwind: rdRead raises it)
    st->L = outerL;
    if (data == NULL || error) { r->eof = true; return false; }
    if (r->pos > 0) { r->buf.erase(0, r->pos); r->pos = 0; }
    r->buf.append(data, len);
    r->buf.push_back('\n');
    return true;
}

static NOINLINE bool rdLine(lua_State *L, State *st, Redirect *r, bool keep)    // "l", "L"
{
    for (;;) {
        size_t nl = r->buf.find('\n', r->pos);
        if (nl != std::string::npos) {
            lua_pushlstring(L, r->buf.data() + r->pos, nl - r->pos + (keep ? 1 : 0));
            r->pos = nl + 1;
            return true;
        }
        if (!rdFill(st, r)) break;
    }
    lua_pushnil(L);
    return false;
}

static NOINLINE bool rdAll(lua_State *L, State *st, Redirect *r)               // "a"
{
    while (rdFill(st, r)) {}
    lua_pushlstring(L, r->buf.data() + r->pos, r->buf.size() - r->pos);
    r->pos = r->buf.size();
    return true;
}

static NOINLINE bool rdCount(lua_State *L, State *st, Redirect *r, lua_Integer k)  // a number
{
    if (k <= 0) {                           // 0: "" if there is more, nil at the end
        bool more = r->pos < r->buf.size() || rdFill(st, r);
        if (more) lua_pushliteral(L, ""); else lua_pushnil(L);
        return more;
    }
    size_t want = (size_t)k;
    while (r->buf.size() - r->pos < want && rdFill(st, r)) {}
    size_t take = r->buf.size() - r->pos < want ? r->buf.size() - r->pos : want;
    if (take == 0) { lua_pushnil(L); return false; }
    lua_pushlstring(L, r->buf.data() + r->pos, take);
    r->pos += take;
    return true;
}

// The next character, if it is one of set (two characters), is taken
static bool rdTest2(const std::string &b, size_t &e, const char *set)
{
    if (e < b.size() && (b[e] == set[0] || b[e] == set[1])) { e++; return true; }
    return false;
}

static int rdDigits(const std::string &b, size_t &e, bool hex)
{
    int n = 0;
    while (e < b.size() && (hex ? isxdigit((unsigned char)b[e]) : isdigit((unsigned char)b[e]))) { e++; n++; }
    return n;
}

// "n", as Lua's l_getn: after blanks, the longest prefix that can be a
// numeral, one character at a time (what does not fit stays in the input)
static NOINLINE bool rdNumber(lua_State *L, State *st, Redirect *r)
{
    for (;;) {                              // blanks, line ends included
        while (r->pos < r->buf.size() && isspace((unsigned char)r->buf[r->pos])) r->pos++;
        if (r->pos < r->buf.size()) break;
        if (!rdFill(st, r)) { lua_pushnil(L); return false; }
    }
    const std::string &b = r->buf;          // a numeral is in one line: no refills from here
    size_t e = r->pos;
    int count = 0;
    bool hex = false;
    rdTest2(b, e, "-+");
    if (rdTest2(b, e, "00")) {
        if (rdTest2(b, e, "xX")) hex = true;
        else count = 1;
    }
    count += rdDigits(b, e, hex);
    if (rdTest2(b, e, "..")) count += rdDigits(b, e, hex);
    if (count > 0 && rdTest2(b, e, hex ? "pP" : "eE")) {
        rdTest2(b, e, "-+");
        rdDigits(b, e, false);
    }
    if (e - r->pos > 200) {                 // as Lua's L_MAXLENNUM: fails, the 201st taken
        r->pos += 201;
        lua_pushnil(L);
        return false;
    }
    lua_pushlstring(L, b.data() + r->pos, e - r->pos);
    r->pos = e;
    if (lua_stringtonumber(L, lua_tostring(L, -1)) != 0) { lua_remove(L, -2); return true; }
    lua_pop(L, 1);
    lua_pushnil(L);
    return false;
}

// The formats from first on (none: "l"), as io.read: a value each, up to the
// first that fails (nil); a format is checked when its turn comes, as Lua
// does.  A Rexx condition reading is a Lua error.
static int rdRead(lua_State *L, State *st, Redirect *r, int first)
{
    if (lua_gettop(L) < first) lua_pushliteral(L, "l");
    int top = lua_gettop(L), n = 0;
    for (int i = first; i <= top; i++) {
        bool ok;
        luaL_checkstack(L, 2, "too many formats");
        r->L = L;
        if (lua_type(L, i) == LUA_TNUMBER) {
            lua_Integer k = luaL_checkinteger(L, i);
            ok = rdCount(L, st, r, k < 0 ? LUA_MAXINTEGER : k);      // Lua: (size_t)k, all of it
        }
        else {
            const char *f = luaL_checkstring(L, i);
            if (*f == '*') f++;                 // Lua 5.1's "*l"
            if (*f == '\0' || strchr("nlLa", *f) == NULL) return luaL_argerror(L, i, "invalid format");
            ok = *f == 'n' ? rdNumber(L, st, r) : *f == 'a' ? rdAll(L, st, r) : rdLine(L, st, r, *f == 'L');
        }
        n++;
        if (st->unwind) return unwindError(L);
        if (st->tc->CheckCondition()) { pushRexxCondition(L, st->tc); return lua_error(L); }
        if (!ok) break;
    }
    return n;
}

static int rd_ioread(lua_State *L)                  // io.read(...)
{
    State *st;
    Redirect *r = redirectOf(L, &st);
    if (rdCurrent(L, true) == 0) return rdDelegate(L, "read");
    return rdRead(L, st, r, 1);
}

static int rd_fread(lua_State *L)                   // f:read(...)
{
    State *st;
    Redirect *r = redirectOf(L, &st);
    return rdRead(L, st, r, 2);
}

// The iterator of io.lines() and f:lines(): upvalues the command's number,
// how many formats, the formats
static int rd_linesnext(lua_State *L)
{
    State *st;
    Redirect *r = redirectOf(L, &st);
    int nf = (int)lua_tointeger(L, lua_upvalueindex(2));
    lua_settop(L, 0);
    luaL_checkstack(L, nf + 2, "too many formats");
    for (int i = 1; i <= nf; i++) lua_pushvalue(L, lua_upvalueindex(2 + i));
    return rdRead(L, st, r, 1);
}

static int rdLines(lua_State *L, int first)
{
    State *st;
    redirectOf(L, &st);
    int nf = lua_gettop(L) - first + 1;
    if (nf < 0) nf = 0;
    luaL_argcheck(L, nf <= 250, first + 250, "too many arguments");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushinteger(L, nf);
    for (int i = 0; i < nf; i++) lua_pushvalue(L, first + i);
    lua_pushcclosure(L, rd_linesnext, 2 + nf);
    return 1;
}

static int rd_iolines(lua_State *L)                 // io.lines(file, ...): Lua's with a file
{
    State *st;
    redirectOf(L, &st);
    if (!lua_isnoneornil(L, 1) || rdCurrent(L, true) == 0) return rdDelegate(L, "lines");
    return rdLines(L, 2);
}

static int rd_flines(lua_State *L) { return rdLines(L, 2); }          // f:lines(...)

static int rd_fself(lua_State *L)                   // f:flush(): f
{
    State *st;
    redirectOf(L, &st);
    lua_settop(L, 1);
    return 1;
}

static int rd_ftrue(lua_State *L)                   // f:setvbuf(): true
{
    State *st;
    redirectOf(L, &st);
    lua_pushboolean(L, 1);
    return 1;
}

static int rd_fclose(lua_State *L)                  // f:close(), f:seek(): fail, message
{
    State *st;
    redirectOf(L, &st);
    lua_pushnil(L);
    lua_pushstring(L, lua_tostring(L, lua_upvalueindex(2)));
    return 2;
}

static int rd_ftostring(lua_State *L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

// f[name] = a closure of f(L, id at idIdx, extra)
static void rdMethod(lua_State *L, int idIdx, const char *name, lua_CFunction f, int extra)
{
    lua_pushvalue(L, idIdx);
    if (extra) lua_insert(L, -2);           // the extra value was on top: id, extra
    lua_pushcclosure(L, f, 1 + extra);
    lua_setfield(L, -2, name);
}

// The stand-in for io.stdin (kind 0), io.stdout (1) or io.stderr (2): a table
// with the methods of a file that make sense for it
static void rdPushFile(lua_State *L, int idIdx, int kind)
{
    lua_newtable(L);
    if (kind == 0) {
        rdMethod(L, idIdx, "read", rd_fread, 0);
        rdMethod(L, idIdx, "lines", rd_flines, 0);
    }
    else { lua_pushboolean(L, kind == 2); rdMethod(L, idIdx, "write", rd_fwrite, 1); }
    rdMethod(L, idIdx, "flush", rd_fself, 0);
    rdMethod(L, idIdx, "setvbuf", rd_ftrue, 0);
    lua_pushliteral(L, "cannot close standard file"); rdMethod(L, idIdx, "close", rd_fclose, 1);
    lua_pushliteral(L, "cannot seek a redirected standard file"); rdMethod(L, idIdx, "seek", rd_fclose, 1);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, kind == 0 ? "file (ADDRESS LUA input)" : kind == 1 ? "file (ADDRESS LUA output)"
                                                                         : "file (ADDRESS LUA error)");
    lua_pushcclosure(L, rd_ftostring, 1);
    lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "ADDRESS LUA stream");
    lua_setfield(L, -2, "__name");
    lua_pushvalue(L, idIdx);                    // for io.type
    lua_rawsetp(L, -2, &rdMark);
    lua_setmetatable(L, -2);
}

// The stand-in on top goes into t[key] (pops it), and {t, key, old value,
// stand-in} at the end of the list at index 1
static void rdReplace(lua_State *L, int t, const char *key)
{
    lua_createtable(L, 4, 0);
    lua_pushvalue(L, t); lua_rawseti(L, -2, 1);
    lua_pushstring(L, key); lua_rawseti(L, -2, 2);
    lua_pushstring(L, key); lua_rawget(L, t); lua_rawseti(L, -2, 3);
    lua_pushvalue(L, -2); lua_rawseti(L, -2, 4);
    lua_rawseti(L, 1, (lua_Integer)lua_rawlen(L, 1) + 1);
    lua_pushstring(L, key);
    lua_insert(L, -2);
    lua_rawset(L, t);
}

// io[name] = a closure of f(L, the command's number at 4, ctx at 5), if
// io[name] is a function (a sandbox's io may not have it)
static void rdIoFunction(lua_State *L, const char *name, lua_CFunction f)
{
    lua_pushstring(L, name);
    lua_rawget(L, 3);
    bool is = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (!is) return;
    lua_pushvalue(L, 4);
    lua_pushvalue(L, 5);
    lua_pushcclosure(L, f, 2);
    rdReplace(L, 3, name);
}

// Puts the stand-ins of st->redirect in place (raw accesses: a strict _G's
// metamethods do not run), each recorded in the list at 1 (rd_newList) for
// rdRestore as it goes.  In a protected call: it allocates.
static int rd_install(lua_State *L)
{
    Redirect *r = stateOf(L)->redirect;
    lua_settop(L, 1);                                       // 1: the list (rd_newList)
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);    // 2: _G
    lua_pushliteral(L, "io");
    lua_rawget(L, 2);                                       // 3: io (if a table)
    lua_pushinteger(L, r->id);                              // 4: the command's number
    if (r->out) {
        lua_pushvalue(L, 4);
        lua_pushcclosure(L, rd_print, 1);
        rdReplace(L, 2, "print");
    }
    if (!lua_istable(L, 3)) { lua_settop(L, 1); return 1; }
    lua_newtable(L);                                        // 5: ctx (rdCtx)
    static const char *const saved[][2] = {
        {"write", "write"}, {"read", "read"}, {"lines", "lines"}, {"output", "output"},
        {"input", "input"}, {"type", "type"}, {"close", "close"}, {"stdin", "rIn"}, {"stdout", "rOut"}, {"stderr", "rErr"}};
    for (const auto &k : saved) {
        lua_pushstring(L, k[1]);
        lua_pushstring(L, k[0]);
        lua_rawget(L, 3);
        lua_rawset(L, 5);
    }
    static const struct { bool Redirect::*on; int kind; const char *sub, *name; } files[] = {
        {&Redirect::in, 0, "sIn", "stdin"}, {&Redirect::out, 1, "sOut", "stdout"},
        {&Redirect::err, 2, "sErr", "stderr"}};
    for (const auto &f : files) {
        if (!(r->*f.on)) continue;
        rdPushFile(L, 4, f.kind);
        lua_pushstring(L, f.sub);
        lua_pushvalue(L, -2);
        lua_rawset(L, 5);
        rdReplace(L, 3, f.name);
    }
    if (r->out || r->err) {
        rdIoFunction(L, "write", rd_iowrite);
        rdIoFunction(L, "output", rd_iooutput);
    }
    if (r->in) {
        rdIoFunction(L, "read", rd_ioread);
        rdIoFunction(L, "lines", rd_iolines);
        rdIoFunction(L, "input", rd_ioinput);
    }
    rdIoFunction(L, "type", rd_iotype);
    rdIoFunction(L, "close", rd_ioclose);
    lua_settop(L, 1);
    return 1;
}

static int rd_newList(lua_State *L)
{
    lua_newtable(L);
    return 1;
}

// Puts back what rd_install replaced (the list at index list), last first,
// where the stand-in is still there.  Called directly, not in a protected
// call: raw reads, and raw writes to keys that exist, raise no errors, and
// after a Ctrl-C the stop hook would end a protected call before it runs.
static void rdRestore(lua_State *L, int list)
{
    if (!lua_checkstack(L, 6)) return;
    for (lua_Integer i = (lua_Integer)lua_rawlen(L, list); i >= 1; i--) {
        lua_rawgeti(L, list, i);                            // e
        lua_rawgeti(L, -1, 1);                              // e t
        lua_rawgeti(L, -2, 2);                              // e t key
        lua_pushvalue(L, -1);
        lua_rawget(L, -3);                                  // e t key now
        lua_rawgeti(L, -4, 4);                              // e t key now stand-in
        bool ours = lua_rawequal(L, -1, -2) != 0;
        lua_pop(L, 2);                                      // e t key
        if (ours) { lua_rawgeti(L, -3, 3); lua_rawset(L, -3); }
        else lua_pop(L, 1);
        lua_pop(L, 2);
    }
}

/* ------------------------------------------------------------------------ */
/* ADDRESS LUA (docs/design.md, *ADDRESS LUA*)                                */
/* ------------------------------------------------------------------------ */

// RC of a command that could not run (FAILURE) or failed (ERROR)
enum { ADDRESS_COMPILE = -1, ADDRESS_NOSTATE = -2, ADDRESS_RUNTIME = 1 };

static RexxObjectPtr addressCondition(RexxExitContext *c, RexxStringObject command, int rc,
                                      const std::string &message)
{
    c->RaiseCondition(rc < 0 ? "FAILURE" : "ERROR", command, c->NewString(message.data(), message.size()),
                      c->WholeNumber(rc));
    return c->WholeNumber(rc);
}

// The command runs in .lua: "return command" if that compiles, else the
// command as a chunk (as ~eval).  RC 0, the results in RESULT (dropped if
// none, the value if one, an Array if several); an error raises FAILURE
// (cannot compile, RC -1; no usable .lua, RC -2) or ERROR (a Lua error while
// running, RC 1), with Lua's message (and traceback) as condition('A').
// With WITH, the stand-ins of print, io.write... while it runs (rd_install).
static RexxObjectPtr RexxEntry addressLua(RexxExitContext *c, RexxStringObject address,
                                           RexxStringObject command, RexxIORedirectorContext *io)
{
    RexxThreadContext *tc = c->threadContext;
    drain(tc);
    RexxObjectPtr lua = tc->DirectoryAt(tc->GetLocalEnvironment(), "LUA");
    // a .LuaState (by a method of its own, not by class name: see luaObjectClass)
    State *st = lua == NULLOBJECT || !tc->HasMethod(lua, "LUA.STATS") ? NULL : (State *)tc->ObjectToCSelf(lua);
    if (st == NULL || st->kind != KIND_STATE)
        return addressCondition(c, command, ADDRESS_NOSTATE, ".lua is not a LuaState");
    if (foreignThread(st))
        return addressCondition(c, command, ADDRESS_NOSTATE, "the LuaState of the Lua that runs ooRexx "
                                "can only be used from that Lua's thread");
    Enter e(st, tc, lua);
    if (st->closed) return addressCondition(c, command, ADDRESS_NOSTATE, "the LuaState is closed");
    releasePending(st);
    lua_State *L = st->L;
    int top = lua_gettop(L);
    const char *code = c->StringData(command);
    size_t length = c->StringLength(command);
    std::string ret = std::string("return ") + std::string(code, length);
    if (luaL_loadbuffer(L, ret.data(), ret.size(), "=ADDRESS LUA") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, code, length, "=ADDRESS LUA") != LUA_OK) {
            size_t len;
            const char *s = lua_tolstring(L, -1, &len);
            std::string m = s != NULL ? std::string(s, len) : std::string("(error object)");
            lua_settop(L, top);
            return addressCondition(c, command, ADDRESS_COMPILE, m);
        }
    }
    std::string error;
    Redirect rd;
    bool redirected = io != NULL && io->IsRedirectionRequested();
    if (redirected) {
        rd.io = io;
        rd.in = io->IsInputRedirected() != 0;
        rd.out = io->IsOutputRedirected() != 0;
        rd.err = io->IsErrorRedirected() != 0;
        rd.id = ++st->redirects;
        rd.prev = st->redirect;
        st->redirect = &rd;
        if (st->running.load() == 0) st->interrupted.store(false);   // as pcallQuiet: a signal
                                                                     // that came as the last call ended
        bool ok = false;
        lua_pushcfunction(L, rd_newList);
        if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
            lua_insert(L, top + 1);                         // the list, under the chunk
            lua_pushcfunction(L, rd_install);
            lua_pushvalue(L, top + 1);
            ok = lua_pcall(L, 1, 0, 0) == LUA_OK;
            if (!ok) rdRestore(L, top + 1);                 // what it put in place before failing
        }
        if (!ok) {                                          // out of memory, or interrupted
            const char *s = lua_tostring(L, -1);
            error = s != NULL ? s : "(error object)";
            st->redirect = rd.prev;
            lua_settop(L, top);
            return addressCondition(c, command, ADDRESS_RUNTIME, error);
        }
    }
    LuaFailure f;
    int n = pcallQuiet(st, 0, f);
    // a HALT in a callback (its error, back here): Error 4.1, as ~eval.  Any
    // other Rexx error is a Lua error of the command: ERROR, with its text
    RexxObjectPtr cond = n < 0 && !st->unwind ? conditionOf(L, -1) : NULLOBJECT;
    bool halt = cond != NULLOBJECT && isHalt(tc, cond);
    if (redirected) {
        rdRestore(L, top + 1);
        lua_remove(L, top + 1);
        st->redirect = rd.prev;
    }
    if (st->unwind) {                       // a SIGNAL past the bridge (guarded): it goes on
        lua_settop(L, top);
        rethrowUnwind(st);
    }
    if (takeInterrupt(st) && !halt) {           // Ctrl-C: the HALT comes after the command
        tc->HaltThread();
        if (n < 0) { lua_settop(L, top); return c->WholeNumber(ADDRESS_RUNTIME); }
    }
    if (n < 0) {
        lua_settop(L, top);
        if (halt) {                             // (a HALT raised here would be ignored
            c->RaiseException1(Rexx_Error_Program_interrupted_condition, c->String("HALT"));
            return NULLOBJECT;                  // when not trapped)
        }
        return addressCondition(c, command, ADDRESS_RUNTIME, f.full());
    }
    if (n == 0) { c->DropContextVariable("RESULT"); return c->WholeNumber(0); }
    RexxObjectPtr r = n == 1 ? firstResult(st, 1) : allResults(st, n);
    lua_settop(L, top);
    if (r == NULLOBJECT || tc->CheckCondition()) return NULLOBJECT;    // a conversion raised a Rexx error
    c->SetContextVariable("RESULT", r);
    return c->WholeNumber(0);
}

// .LuaState~"LUA.ADDRESS" (lua.cls, when it is loaded): the LUA environment,
// in an instance the Lua module did not make (those have it from the start,
// newInst).  Once, before any command: AddCommandEnvironment does not take
// the interpreter's lock (docs/design.md, *ADDRESS LUA*).
// Once per instance, even if called again (it is a public method): marked in
// .local, which belongs to the instance and ends with it.  (The kernel lock
// is held from the test to the mark.)
RexxMethod0(RexxObjectPtr, Lua_Address)
{
    RexxThreadContext *tc = context->threadContext;
    if (instOfContext(tc) != NULL) return NULLOBJECT;
    RexxDirectoryObject local = tc->GetLocalEnvironment();
    if (tc->DirectoryAt(local, "LUA.ADDRESS") != NULLOBJECT) return NULLOBJECT;
    tc->DirectoryPut(local, tc->True(), "LUA.ADDRESS");
    tc->AddCommandEnvironment("LUA", (REXXPFN)addressLua, REDIRECTING_COMMAND_ENVIRONMENT);
    return NULLOBJECT;
}

/* ------------------------------------------------------------------------ */
/* Package                                                                    */
/* ------------------------------------------------------------------------ */

RexxMethodEntry rexxlua_methods[] = {
    REXX_METHOD(LuaState_New,       LuaState_New),
    REXX_METHOD(Lua_Graveyard,      Lua_Graveyard),
    REXX_METHOD(Lua_AdoptNext,      Lua_AdoptNext),
    REXX_METHOD(Lua_Address,        Lua_Address),
    REXX_METHOD(LuaState_Close,     LuaState_Close),
    REXX_METHOD(LuaState_IsClosed,  LuaState_IsClosed),
    REXX_METHOD(LuaState_Eval,      LuaState_Eval),
    REXX_METHOD(LuaState_Global,    LuaState_Global),
    REXX_METHOD(LuaState_Require,   LuaState_Require),
    REXX_METHOD(LuaState_Box,       LuaState_Box),
    REXX_METHOD(LuaState_AddCommandEnvironment,    LuaState_AddCommandEnvironment),
    REXX_METHOD(LuaState_RemoveCommandEnvironment, LuaState_RemoveCommandEnvironment),
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
    REXX_METHOD(LuaObj_Supplier,    LuaObj_Supplier),
    REXX_METHOD(LuaObj_MakeStringTable, LuaObj_MakeStringTable),
    REXX_METHOD(LuaObj_Type,        LuaObj_Type),
    REXX_METHOD(LuaObj_Operator,    LuaObj_Operator),
    REXX_METHOD(LuaObj_HashCode,    LuaObj_HashCode),
    REXX_LAST_METHOD()
};

RexxPackageEntry rexxlua_package_entry = {
    STANDARD_PACKAGE_HEADER
    REXX_INTERPRETER_5_0_0,
    "rexxlua",
    "0.2.0",
    NULL, NULL,
    NULL,
    rexxlua_methods
};

OOREXX_GET_PACKAGE(rexxlua);
