fibers(1) -- Fiber support for v8 and Node
==========================================

INSTALLING
----------

To install `node-fibers` just use `npm`. The exact process depends on your
version of Node, so read below.

### If you're using node < 0.5.2:

It's recommended that you install `node-fibers` globally, as it includes a
wrapper script which you must run in. Furthermore you must install from the 0.5
version of `node-fibers`:

* `npm install -g fibers@0.5`
* Ensure `node-fibers` can be found in your $PATH (this should be true unless
	you symlinked `node` somewhere).
* Ensure the global `node_modules` directory is in your $NODE_PATH. You might
	have done this when you installed `npm`, but probably didn't. If this is too
	difficult you can also just do this:
	``NODE_ROOT=$(echo 'console.log(require("path").resolve(process.execPath, "..", ".."))' | node); ln -s $(npm list -g | head -n1)/node_modules/fibers $NODE_ROOT/lib/node/``

### Or if you're using node >= 0.5.2:

* `npm install fibers`
* You're done!

Only Linux and OS X environments are supported. Windows support is totally
possible, but I'm not going to do it for you. You may be able to build this on
Windows in cygwin just by messing with the makefiles.


GETTING STARTED
---------------

If you intend to use fibers, be sure to run `node-fibers` instead of `node`
**note: this only applies for node<0.5.2 and node-fibers 0.5.x**. After that
just `require('fibers');` in any Javascript file and you will have fiber
support.


EXAMPLES
--------

The examples below describe basic use of `Fiber`, but note that it is **not
recommended** to use `Fiber` without an abstraction in between your code and
fibers. See "FUTURES" below for additional information.

### Sleep
This is a quick example of how you can write sleep() with fibers. Note that
while the sleep() call is blocking inside the fiber, node is able to handle
other events.

	$ cat sleep.js

```javascript
require('fibers');

function sleep(ms) {
	var fiber = Fiber.current;
	setTimeout(function() {
		fiber.run();
	}, ms);
	yield();
}

Fiber(function() {
	console.log('wait... ' + new Date);
	sleep(1000);
	console.log('ok... ' + new Date);
}).run();
console.log('back in main');
```

	$ node sleep.js
	wait... Fri Jan 21 2011 22:42:04 GMT+0900 (JST)
	back in main
	ok... Fri Jan 21 2011 22:42:05 GMT+0900 (JST)


### Incremental Generator
Yielding execution will resume back in the fiber right where you left off. You
can also pass values back and forth through yield() and run(). Again, the node
event loop is never blocked while this script is running.

	$ cat generator.js

```javascript
require('fibers');

var inc = Fiber(function(start) {
	var total = start;
	while (true) {
		total += yield(total);
	}
});

for (var ii = inc.run(1); ii <= 10; ii = inc.run(1)) {
	console.log(ii);
}
```

	$ node generator.js
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


### Fibonacci Generator
Expanding on the incremental generator above, we can create a generator which
returns a new Fibonacci number with each invocation. You can compare this with
the [ECMAScript Harmony
Generator](http://wiki.ecmascript.org/doku.php?id=harmony:generators) Fibonacci
example.

	$ cat fibonacci.js

```javascript
require('fibers');

// Generator function. Returns a function which returns incrementing
// Fibonacci numbers with each call.
function Fibonacci() {
	// Create a new fiber which yields sequential Fibonacci numbers
	var fiber = Fiber(function() {
		yield(0); // F(0) -> 0
		var prev = 0, curr = 1;
		while (true) {
			yield(curr);
			var tmp = prev + curr;
			prev = curr;
			curr = tmp;
		}
	});
	// Return a bound handle to `run` on this fiber
	return fiber.run.bind(fiber);
}

// Initialize a new Fibonacci sequence and iterate up to 1597
var seq = Fibonacci();
for (var ii = seq(); ii <= 1597; ii = seq()) {
	console.log(ii);
}
```

	$ node fibonacci.js
	0
	1
	1
	2
	3
	5
	8
	13
	21
	34
	55
	89
	144
	233
	377
	610
	987
	1597


### Basic Exceptions
Fibers are exception-safe; exceptions will continue travelling through fiber
boundaries:

	$ cat error.js

```javascript
require('fibers');

var fn = Fiber(function() {
	console.log('async work here...');
	yield();
	console.log('still working...');
	yield();
	console.log('just a little bit more...');
	yield();
	throw new Error('oh crap!');
});

try {
	while (true) {
		fn.run();
	}
} catch(e) {
	console.log('safely caught that error!');
	console.log(e.stack);
}
console.log('done!');
```

	$ node error.js
	async work here...
	still working...
	just a little bit more...
	safely caught that error!
	Error: oh crap!
			at error.js:11:9
	done!


FUTURES
-------

Using the `Fiber` class without an abstraction in between your code and the raw
API is **not recommended**. `Fiber` is meant to implement the smallest amount of
functionality in order make possible many different programming patterns. This
makes the `Fiber` class relatively lousy to work with directly, but extremely
powerful when coupled with a decent abstraction. There is no right answer for
which abstraction is right for you and your project. Included with `node-fibers`
is an implementation of "futures" which is fiber-aware.  Usage of this library
is documented below. Other externally-maintained options include
[0ctave/node-sync](https://github.com/0ctave/node-sync) and
[lm1/node-fibers-promise](https://github.com/lm1/node-fibers-promise). However
you **should** feel encouraged to be creative with fibers and build a solution
which works well with your project. For instance, `Future` is not a good
abstraction to use if you want to build a generator function (see Fibonacci
example above).

Using `Future` to wrap existing node functions. At no point is the node event
loop blocked:

	$ cat ls.js

```javascript
var Future = require('fibers/future'), wait = Future.wait;
var fs = require('fs');

// This wraps existing functions assuming the last argument of the passed
// function is a callback. The new functions created immediately return a
// future and the future will resolve when the callback is called (which
// happens behind the scenes).
var readdir = Future.wrap(fs.readdir);
var stat = Future.wrap(fs.stat);

Fiber(function() {
	// Get a list of files in the directory
	var fileNames = readdir('.').wait();
	console.log('Found '+ fileNames.length+ ' files');

	// Stat each file
	var stats = [];
	for (var ii = 0; ii < fileNames.length; ++ii) {
		stats.push(stat(fileNames[ii]));
	}
	wait(stats);

	// Print file size
	for (var ii = 0; ii < fileNames.length; ++ii) {
		console.log(fileNames[ii]+ ': '+ stats[ii].get().size);
	}
}).run();
```

	$ node ls.js 
	Found 11 files
	bin: 4096
	fibers.js: 1708
	.gitignore: 37
	README.md: 8664
	future.js: 5833
	.git: 4096
	LICENSE: 1054
	src: 4096
	ls.js: 860
	Makefile: 436
	package.json: 684


The future API is designed to make it easy to move between classic
callback-style code and fiber-aware waiting code:

	$ cat sleep.js

```javascript
var Future = require('fibers/future'), wait = Future.wait;

// This function returns a future which resolves after a timeout. This
// demonstrates manually resolving futures.
function sleep(ms) {
	var future = new Future;
	setTimeout(function() {
		future.return();
	}, ms);
	return future;
}

// You can create functions which automatically run in their own fiber and
// return futures that resolve when the fiber returns (this probably sounds
// confusing.. just play with it to understand).
var calcTimerDelta = function(ms) {
	var start = new Date;
	sleep(ms).wait();
	return new Date - start;
}.future(); // <-- important!

// And futures also include node-friendly callbacks if you don't want to use
// wait()
calcTimerDelta(2000).resolve(function(err, val) {
	console.log('Set timer for 2000ms, waited '+ val+ 'ms');
});
```

	$ node sleep.js
	Set timer for 2000ms, waited 2009ms


API DOCUMENTATION
-----------------
Fiber's definition looks something like this:

```javascript
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
 *
 * Note also that `yield` is a reserved word in Javascript. This is normally
 * not an issue, however if using strict mode you will not be able to call
 * yield() globally. Instead call `Fiber.yield()`.
 */
function yield(param) {
	[native code]
}
Fiber.yield = yield;

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
 * This is accomplished by causing yield() to throw an exception, and any
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
```

GARBAGE COLLECTION
------------------

If you intend to build generators, iterators, or "lazy lists", you should be
aware that all fibers must eventually unwind. This is implemented by causing
yield() to throw unconditionally when the library is trying to unwind your
fiber-- either because reset() was called, or all handles to the fiber were lost
and v8 wants to delete it.

Something like this will, at some point, cause an infinite loop in your
application:

```javascript
var fiber = Fiber(function() {
	while (true) {
		try {
			yield();
		} catch(e) {}
	}
});
fiber.run();
```

If you either call reset() on this fiber, or the v8 garbage collector decides it
is no longer in use, the fiber library will attempt to unwind the fiber by
causing all calls to yield() to throw. However, if you catch these exceptions
and continue anyway, an infinite loop will occur.

There are other garbage collection issues that occur with misuse of fiber
handles. If you grab a handle to a fiber from within itself, you should make
sure that the fiber eventually unwinds. This application will leak memory:

```javascript
var fiber = Fiber(function() {
	var that = Fiber.current;
	yield();
}
fiber.run();
fiber = undefined;
```

There is no way to get back into the fiber that was started, however it's
impossible for v8's garbage collector to detect this. With a handle to the fiber
still outstanding, v8 will never garbage collect it and the stack will remain in
memory until the application exits.

Thus, you should take care when grabbing references to `Fiber.current`.
