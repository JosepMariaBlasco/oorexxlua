/* 08-command-environments.rex -- Lua functions as ADDRESS environments.
   Its exact output is 08-command-environments.out.
   Run: examples/run-one.sh 08-command-environments.rex */

-- 1. A calculator: RC is the function's value; ctx sets the caller's RESULT
calc = .lua~eval("return function(command, ctx)" -
  "  local f = load('return ' .. command)" -
  "  if not f then return 1 end" -          -- RC 1: just RC, no condition
  "  ctx:set_var('RESULT', f())" -
  "end")
.lua~addCommandEnvironment("CALC", calc)
address calc
'2^10';        say rc result                       -- 0 1024.0
'(1 + 2) * 7'; say rc result                       -- 0 21
'1 +';         say rc                              -- 1

-- 2. The caller's variables, both ways (a compound name's tail resolved here)
.lua~addCommandEnvironment("TOTAL", .lua~eval("return function(command, ctx)" -
  "  local sum = 0" -
  "  for i = 1, ctx:get_var(command .. '0') do sum = sum + ctx:get_var(command .. i) end" -
  "  ctx:set_var('TOTAL', sum)" -
  "end"))
price.0 = 3; price.1 = 2.50; price.2 = 10; price.3 = 0.25
address total 'price.'
say total                                          -- 12.75

-- 3. ERROR and FAILURE only when the handler asks for them
.lua~addCommandEnvironment("CHECK", .lua~eval("return function(command, ctx)" -
  "  if command ~= 'ok' then ctx:output('no: ' .. command); return ctx:raise_error(8) end" -
  "end"))
signal on error
address check 'ok'; say 'ok:' rc                   -- ok: 0 (output without WITH: .output)
address check 'bad'
say 'not here'
error:
  say condition('C') rc condition('D')             -- ERROR 8 bad

-- 4. A Lua error in the handler is a SYNTAX error where the command was issued
.lua~addCommandEnvironment("BROKEN", .lua~eval("return function() error('oops') end"))
signal on syntax
address broken 'x'
syntax:
  say condition('O')~code condition('O')~additional[2]   -- 98.900 ...: oops

-- 5. Removed: FAILURE RC 30, as an environment that was never there
.lua~removeCommandEnvironment("CALC")
signal on failure
trace off                                          -- (TRACE NORMAL would show the failed command)
address calc '1 + 1'
failure:
  say condition('C') rc                            -- FAILURE 30
exit

::requires "lua.cls"
