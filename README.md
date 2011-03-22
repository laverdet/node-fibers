fibers(1) -- Fiber support for v8 and Node
==========================================

INSTALLING
----------

To install node-fibers:

	npm install fibers

Only Linux and OS X environments are supported. Windows support is theoretically
possible, but not planned.


GETTING STARTED
---------------

If you intend to use fibers, be sure to run `node-fibers` instead of `node`.
After that just `require('fibers');` in any Javascript file and you will have
fiber support.


EXAMPLES
--------

This is a quick example of how you can write sleep() with fibers. Note that
while the sleep() call is blocking inside the fiber, node is able to handle
other events.

	$ cat sleep.js
	require('fibers');
	var print = require('util').print;

	function sleep(ms) {
		var fiber = Fiber.current;
		setTimeout(function() {
			fiber.run();
		}, ms);
		yield();
	}

	Fiber(function() {
		print('wait... ' + new Date + '\n');
		sleep(1000);
		print('ok... ' + new Date + '\n');
	}).run();
	print('back in main\n');

	$ node-fibers sleep.js
	wait... Fri Jan 21 2011 22:42:04 GMT+0900 (JST)
	back in main
	ok... Fri Jan 21 2011 22:42:05 GMT+0900 (JST)


Yielding execution will resume back in the fiber right where you left off. You
can also pass values back and forth through yield() and run().

	$ cat generator.js
	require('fibers');
	var print = require('util').print;

	var inc = Fiber(function(start) {
		var total = start;
		while (true) {
			total += yield(total);
		}
	});

	for (var ii = inc.run(1); ii <= 10; ii = inc.run(1)) {
		print(ii + '\n');
	}

	$ node-fibers generator.js
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


Fibers are exception-safe; exceptions will continue travelling through fiber
boundaries:

	$ cat error.js
	require('fibers');
	var print = require('util').print;

	var fn = Fiber(function() {
		print('async work here...\n');
		yield();
		print('still working...\n');
		yield();
		print('just a little bit more...\n');
		yield();
		throw new Error('oh crap!');
	});

	try {
		while (true) {
			fn.run();
		}
	} catch(e) {
		print('safely caught that error!\n');
		print(e.stack + '\n');
	}
	print('done!\n');

	$ node-fibers error.js
	async work here...
	still working...
	just a little bit more...
	safely caught that error!
	Error: oh crap!
			at error.js:11:9
	done!


You can use fibers to provide synchronous adapters on top of asynchronous
functions:

	$ cat adapter.js
	require('fibers');
	var print = require('util').print;

	// This function runs an asynchronous function from within a fiber as if it
	// were synchronous.
	function asyncAsSync(fn /* ... */) {
		var args = [].slice.call(arguments, 1);
		var fiber = Fiber.current;

		function cb(err, ret) {
			if (err) {
				fiber.throwInto(new Error(err));
			} else {
				fiber.run(ret);
			}
		}

		// Little-known JS features: a function's `length` property is the number
		// of arguments it takes. Node convention is that the last parameter to
		// most asynchronous is a `function callback(err, ret) {}`.
		args[fn.length - 1] = cb;
		fn.apply(null, args);

		return yield();
	}

	var fs = require('fs');
	var Buffer = require('buffer').Buffer;
	Fiber(function() {
		// These are all async functions (fs.open, fs.write, fs.close) but we can
		// use them as if they're synchronous.
		print('opening /tmp/hello\n');
		var file = asyncAsSync(fs.open, '/tmp/hello', 'w');
		var buffer = new Buffer(5);
		buffer.write('hello');
		print('writing to file\n');
		asyncAsSync(fs.write, file, buffer, 0, buffer.length);
		print('closing file\n');
		asyncAsSync(fs.close, file);

		// This is a synchronous function. But note that while this function is
		// running node is totally blocking. Using `asyncAsSync` leaves node
		// available to handle more events.
		var data = fs.readFileSync('/tmp/hello');
		print('file contents: ' +data +'\n');

		// Errors made simple using the magic of exceptions
		try {
			print('deleting /tmp/hello2\n');
			asyncAsSync(fs.unlink, '/tmp/hello2');
		} catch(e) {
			print('caught this exception: ' +e.message +'\n');
		}

		// Cleanup :)
		print('deleting /tmp/hello\n');
		asyncAsSync(fs.unlink, '/tmp/hello');
	}).run();
	print('returning control to node event loop\n');

	$ node-fibers adapter.js
	opening /tmp/hello
	returning control to node event loop
	writing to file
	closing file
	file contents: hello
	deleting /tmp/hello2
	caught this exception: Error: ENOENT, No such file or directory '/tmp/hello2'
	deleting /tmp/hello


DOCUMENTATION
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


GARBAGE COLLECTION
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
