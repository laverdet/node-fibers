node-fibers
===========

Fiber support for v8 and Node.


Building
--------

To build this software:

    make

Only Linux and OS X environments are supported. Windows support is theoretically
possible, but not planned.


Getting Started
---------------

If you intend to use fibers, be sure to start Node with the included
`fiber-shim` script. This is a quick example of what you can do with
node-fibers:

    $ cat sleep.js 
    require('./node-fibers');
    var util = require('util');

    function sleep(ms) {
      var fiber = Fiber.current;
      setTimeout(function() {
        fiber.run();
      }, ms);
      yield();
    }

    Fiber(function() {
      util.print('wait... ' + new Date + '\n');
      sleep(1000);
      util.print('ok... ' + new Date + '\n');
    }).run();
    util.print('back in main\n');

    $ ./fiber-shim node sleep.js 
    wait... Fri Jan 21 2011 22:42:04 GMT+0900 (JST)
    back in main
    ok... Fri Jan 21 2011 22:42:05 GMT+0900 (JST)

Yielding execution will resume back in the fiber right where you left off. You
can also pass values back and forth through yield() and run().

    $ cat generator.js
    var util = require('util');
    require('./node-fibers');

    var inc = Fiber(function(start) {
      var total = start;
      while (true) {
        total += yield(total);
      }
    });

    for (var ii = inc.run(1); ii <= 10; ii = inc.run(1)) {
      util.print(ii + '\n');
    }

    $ ./fiber-shim node generator.js 
    1
    2
    3
    4
    5
    6
    7
    8
    9
    10


Documentation
-------------
Fiber's definition looks something like this:

    /**
     * Instantiate a new Fiber. You may invoke this either as a function or as
     * a constructor; the behavior is the same.
     *
     * When run() is called on this fiber for the first time, `fn` will be
     * invoked as the first frame on a new stack. Execution will continue on
     * this new stack until `fn` returns, or yield() is called.
     *
     * After the function returns the fiber is reset to original state and
     * may be restarted with another call to run().
     */
    function Fiber(fn) {
      [native code]
    }

    /**
     * `Fiber.current` will contain the currently-running Fiber. It will be
     * `undefined` if there is no fiber (i.e. the main stack of execution).
     *
     * See "Garbage Collection" for more information on responsible use of
     * `Fiber.current`.
     */
    Fiber.current = undefined;

    /**
     * yield() will halt execution of the current fiber and return control back
     * to original caller of run(). If an argument is supplied to yield, run()
     * will return that value.
     *
     * When run() is called again, yield() will return.
     *
     * Note that this function is a global to allow for correct garbage
     * collection. This results in no loss of functionality because it is only
     * valid to yield from the currently running fiber anyway.
     */
    function yield(param) {
      [native code]
    }

    /**
     * run() will start execution of this Fiber, or if it is currently yielding,
     * it will resume execution. If an argument is supplied, this argument will
     * be passed to the fiber, either as the first parameter to the main
     * function [if the fiber has not been started] or as the return value of
     * yield() [if the fiber is currently yielding].
     *
     * This function will return either the parameter passed to yield(), or the
     * returned value from the fiber's main function.
     */
    Fiber.prototype.run = function(param) {
      [native code]
    }

    /**
     * reset() will terminate a running Fiber and restore it to its original
     * state, as if it had returned execution.
     *
     * This is accomplished by causing yield() to throw an execution, and any
     * futher calls to yield() will also throw an exception. This continues
     * until the fiber has completely unwound and returns.
     *
     * If the fiber returns a value it will be returned by reset().
     *
     * If the fiber is not running, reset() will have no effect.
     */
    Fiber.prototype.reset = function() {
      [native code]
    }

    /**
     * throwInto() will cause a currently yielding fiber's yield() call to
     * throw instead of return gracefully. This can be useful for notifying a
     * fiber that you are no longer interested in its task, and that it should
     * give up.
     *
     * Note that if the fiber does not handle the exception it will continue to
     * bubble up and throwInto() will throw the exception right back at you.
     */
    Fiber.prototype.throwInto = function(exception) {
      [native code]
    }


Garbage Collection
------------------

If you intend to use "lazy lists", you should be aware that all fibers must
eventually unwind. This is implemented by causing yield() to throw
unconditionally when the library is trying to unwind your fiber-- either
because reset() was called, or all handles to the fiber were lost and v8 wants
to delete it.

Something like this will, at some point, cause an infinite loop in your
application:

    var fiber = Fiber(function() {
      while (true) {
        try {
          yield();
        } catch(e) {}
      }
    });
    fiber.run();

If you either call reset() on this fiber, or the v8 garbage collector decides it
is no longer in use, the fiber library will attempt to unwind the fiber by
causing all calls to yield() to throw. However, if you catch these exceptions
and continue anyway, an infinite loop will occur.

There are other garbage collection issues that occur with misuse of fiber
handles. If you grab a handle to a fiber from within itself, you should make
sure that the fiber eventually unwinds. This application will leak memory:

    var fiber = Fiber(function() {
      var that = Fiber.current;
      yield();
    }
    fiber.run();
    fiber = undefined;

There is no way to get back into the fiber that was started, however it's
impossible for v8's garbage collector to detect this. With a handle to the fiber
still outstanding, v8 will never garbage collect it and the stack will remain in
memory until the application exits.

Thus, you should take care when grabbing references to `Fiber.current`.
