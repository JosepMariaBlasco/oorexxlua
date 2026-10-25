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
 * condition it raises; the test fails if none is raised.
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

/** assertOutputs(dir, minimum, what)
 * Runs each .rex (with rexx) and .lua (with the standalone lua in ORXLUA_LUA)
 * file in dir, in its own process with dir as the current directory, and
 * compares what it prints (stdout and stderr) with the NAME.out next to it.
 * At least minimum files must be there.  Used by Examples.testGroup and
 * Probes.testGroup.
 */
::method assertOutputs
  use strict arg dir, minimum, what
  names = .array~new
  do f over .File~new(dir)~listFiles
    if f~name~endsWith(".rex") | f~name~endsWith(".lua") then names~append(f~absolutePath)
  end
  names = names~sort
  self~assertTrue(names~items >= minimum, what "found in" dir)
  lua = value("ORXLUA_LUA", , "ENVIRONMENT")

  failures = .array~new
  do path over names
    outFile = path~left(path~length - 4) || ".out"
    if \SysFileExists(outFile) then do
      failures~append(path": no" outFile)
      iterate
    end
    s = .stream~new(outFile)
    expected = s~arrayIn~makeString("L", "0a"x)
    s~close
    runner = "rexx"
    if path~endsWith(".lua") then do
      if lua == "" then do
        failures~append(path": no standalone lua (ORXLUA_LUA)")
        iterate
      end
      runner = '"'lua'"'
    end
    got = self~outputOf(dir, path, runner)~makeString("L", "0a"x)
    if got == expected then self~assertTrue(.true)
    else failures~append(.File~new(path)~name || "0a"x ||,
                         "  expected: [" || expected~changeStr("0a"x, "|") || "]" || "0a"x ||,
                         "  got:      [" || got~changeStr("0a"x, "|") || "]")
  end
  if failures~items > 0 then
    self~fail(failures~items what"(s) differ:" || "0a"x || failures~makeString("L", "0a"x))

/** outputOf(dir, path [, runner])
 * Runs the file path with runner (default rexx), in its own process with dir
 * as the current directory, and returns the lines it prints (stdout and
 * stderr) as an array.  Used by assertOutputs, and by tests that need an
 * instance of their own (one where lua.cls has not been loaded yet).
 */
::method outputOf
  use strict arg dir, path, runner = "rexx"
  -- librexxlua is found by name through the library path; macOS drops
  -- DYLD_* variables when it starts /bin/sh (ADDRESS SYSTEM), so the command
  -- sets it itself
  parse upper source platform .
  libvar = ""
  if platform~pos("DARWIN") > 0 | platform~pos("MAC") > 0 then
    libvar = "DYLD_LIBRARY_PATH='"value("ORXLUA_BUILD", , "ENVIRONMENT")"' "
  if platform~abbrev("WIN") then       -- cmd.exe
    cmd = 'cd /d "'dir'" && 'runner '"'path'" 2>&1'
  else
    cmd = 'cd "'dir'" && 'libvar || runner '"'path'" 2>&1'
  out = .array~new
  address system cmd with output using (out)
  do i = 1 to out~items                -- Windows: lines may end in CR
    if out[i]~right(1) == "0d"x then out[i] = out[i]~left(out[i]~length - 1)
  end
  return out

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
