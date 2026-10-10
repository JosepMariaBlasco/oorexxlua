-- temporary: texlua on Windows (removed once found)
io.stdout:setvbuf("no")
print("1 require"); local rexx = require "oorexx"
print("2 eval", rexx.eval("6*7"))
print("3 routine", rexx.routine("return 1")())
print("4 end")
