/*----------------------------------------------------------------------------*/
/* lua-over-registered.rex: run by CommandHandlers.testGroup                  */
/* (test_lua_cls_stacks_over_a_registered_lua), in an instance of its own.    */
/* A handler registered as LUA before lua.cls is loaded stays under the       */
/* bridge's ADDRESS LUA (StackCommandEnvironment): deregister uncovers it.    */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0.                            */
/*----------------------------------------------------------------------------*/

.CommandHandler~register("LUA", .Mine~new)
address lua "return 1"
say "before:" rc
.context~package~loadPackage("lua.cls")
address lua "return 6 * 7"
say "loaded:" rc result
.CommandHandler~deregister("LUA")
address lua "return 2"
say "deregistered:" rc
.CommandHandler~deregister("LUA")
trace off                               -- no trace of the failure (RC 30)
address lua "return 3"
say "unknown:" rc

::class Mine subclass CommandHandler
::method command
  return 1000
