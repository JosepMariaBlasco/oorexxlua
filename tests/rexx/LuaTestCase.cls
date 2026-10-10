/*----------------------------------------------------------------------------*/
/* LuaTestCase.cls: the base class of the bridge's ooRexxUnit test groups,   */
/* and the routines they share.                                               */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0.                            */
/*----------------------------------------------------------------------------*/

::requires 'ooTest.frm'      -- ooRexxUnit (scripts/setup-oorexxunit.sh)
::requires 'lua.cls'         -- the bridge

::class LuaTestCase subclass ooTestCase public

/** errorOf(receiver, messageName [, args...])
 * Sends the message and returns the first line of the message of the SYNTAX
 * condition it raises (the bridge's Lua errors go on with a traceback); the
 * test fails if none is raised.
 */
::method errorOf
  use strict arg receiver, name, ...
  args = arg(3, "A")
  signal on syntax
  .message~new(receiver, name, "A", args)~send
  signal off syntax
  self~fail("no SYNTAX condition raised by message" name)
syntax:
  return condition("O")~message~makeArray("0a"x)[1]

/** rss()
 * The resident memory of this process, in KB: /proc on Linux, ps on macOS,
 * the working set on Windows.  0 if it cannot be measured.
 */
::routine rss public
  f = "/proc/self/status"
  if \SysFileExists(f) then do
    pid = SysQueryProcess("PID")
    parse upper source platform .
    if platform~abbrev("WIN") then
      cmd = 'powershell -NoProfile -Command "[long]((Get-Process -Id' pid').WorkingSet64 / 1KB)"'
    else
      cmd = "ps -o rss= -p" pid
    out = .array~new
    address system cmd with output using (out)
    rss = word(out~makeString("L", " "), 1)
    return (datatype(rss, "W"))~?(rss, 0)
  end
  s = .stream~new(f)                    -- (~lines is 0 on /proc files)
  lines = s~arrayIn; s~close
  do line over lines
    parse value translate(line, " ", "09"x) with key ":" value .
    if key = "VmRSS" then return value
  end
  return 0

/** churn()
 * Makes garbage on the Rexx side, so that ooRexx runs the pending uninits,
 * and then calls the bridge, which drains its graveyard.
 */
::routine churn public
  do i = 1 to 200000; a = .array~new(10); end
  x = .lua~eval("1")
  return
