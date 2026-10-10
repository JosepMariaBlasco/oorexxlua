/* threads.rex -- several Rexx threads in one Lua at once, with callbacks into
   Rexx (docs/design.md, *Threads*): the state's lock serializes them        */
threads = 8; n = 2000
.lua~eval("counter = 0; function bump(f, i) counter = counter + 1; return f(i) end")
twice = .routine~new("twice", "return arg(1) * 2")
done = .queue~new
do t = 1 to threads
  .Worker~new~run(t, n, twice, done)
end
ok = waitAll(done, threads)
say "every thread got the right results:" ok
say "every call ran once:" (.lua~global("counter") = threads * n)

-- each thread with a state of its own: no lock is shared
done = .queue~new
do t = 1 to threads
  .Worker~new~own(t, n, done)
end
ok = waitAll(done, threads)
say "threads with states of their own:" ok
exit

waitAll: procedure          -- the results of all the threads (a Queue's pull does not wait)
  use arg done, threads
  do while done~items < threads; call SysSleep 0.01; end
  ok = 1
  do t = 1 to threads; ok = ok & done~pull; end
  return ok

::class Worker
::method run
  use arg t, n, twice, done
  reply
  bump = .lua~global("bump")
  ok = 1
  do i = 1 to n
    if bump~call(twice, i) <> 2 * i then ok = 0
  end
  done~queue(ok)
::method own
  use arg t, n, done
  reply
  s = .LuaState~new
  s~eval("function sq(x) return x * x end")
  sq = s~global("sq")
  ok = 1
  do i = 1 to n
    if sq~call(i) <> i * i then ok = 0
  end
  s~close
  done~queue(ok)

::requires "lua.cls"
