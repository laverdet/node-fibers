"use strict";
var Fiber = require('./fibers');
var util = require('util');
module.exports = Future;
Function.prototype.future = function(detach) {
	var fn = this;
	var ret = function() {
		var future = new FiberFuture(fn, this, arguments);
		if (detach) {
			future.detach();
		}
		return future;
	};
	ret.toString = function() {
		return '<<Future '+ fn+ '.future()>>';
	};
	return ret;
};

var cachedFiberFutures = [];

function getCachedFiberFuture() {
	for (var index = 0; index < cachedFiberFutures.length; index++) {
		var element = cachedFiberFutures[index];
		if(element.fiber === Fiber.current) {
			return element;
		}
	}
}

function addActiveFuture(future) {
	var cachedFiberFuture = getCachedFiberFuture();
	if(!cachedFiberFuture) {
		cachedFiberFuture = {
			activeFutures: [],
			fiber: Fiber.current,
			activeFuturesCtorStacks: []
		};
		cachedFiberFutures.push(cachedFiberFuture);
	}

	cachedFiberFuture.activeFutures.push(future);
	cachedFiberFuture.activeFuturesCtorStacks.push(new Error().stack);
}

function deleteActiveFuture(future) {
	var cachedFiberFuture = getCachedFiberFuture();
	if(cachedFiberFuture) {
		var index = cachedFiberFuture.activeFutures.indexOf(future);
		if (index !== -1) {
			cachedFiberFuture.activeFutures.splice(index, 1);
			cachedFiberFuture.activeFuturesCtorStacks.splice(index, 1);
			cachedFiberFutures = cachedFiberFutures.filter(function(f){
				return f.activeFutures.length;
			});
		}
	}
}

function Future() {
	addActiveFuture(this);
}

/**
 * Run a function(s) in a future context, and return a future to their return value. This is useful
 * for instances where you want a closure to be able to `.wait()`. This also lets you wait for
 * mulitple parallel opertions to run.
 */
Future.task = function(fn) {
	if (arguments.length === 1) {
		return fn.future()();
	} else {
		var future = new Future, pending = arguments.length, error, values = new Array(arguments.length);
		for (var ii = 0; ii < arguments.length; ++ii) {
			arguments[ii].future()().resolve(function(ii, err, val) {
				if (err) {
					error = err;
				}
				values[ii] = val;
				if (--pending === 0) {
					if (error) {
						future.throw(error);
					} else {
						future.return(values);
					}
				}
			}.bind(null, ii));
		}
		return future;
	}
};

/**
 * Wrap node-style async functions to instead return futures. This assumes that the last parameter
 * of the function is a callback.
 *
 * If a single function is passed a future-returning function is created. If an object is passed a
 * new object is returned with all functions wrapped.
 *
 * The value that is returned from the invocation of the underlying function is assigned to the
 * property `_` on the future. This is useful for functions like `execFile` which take a callback,
 * but also return meaningful information.
 *
 * `multi` indicates that this callback will return more than 1 argument after `err`. For example,
 * `child_process.exec()`
 *
 * `suffix` will append a string to every method that was overridden, if you pass an object to
 * `Future.wrap()`. Default is 'Future'.
 *
 * var readFileFuture = Future.wrap(require('fs').readFile);
 * var fs = Future.wrap(require('fs'));
 * fs.readFileFuture('example.txt').wait();
 */
Future.wrap = function(fnOrObject, multi, suffix, stop) {
	if (typeof fnOrObject === 'object') {
		var wrapped = Object.create(fnOrObject);
		for (var ii in fnOrObject) {
			if (wrapped[ii] instanceof Function) {
				wrapped[suffix === undefined ? ii+ 'Future' : ii+ suffix] = Future.wrap(wrapped[ii], multi, suffix, stop);
			}
		}
		return wrapped;
	} else if (typeof fnOrObject === 'function') {
		var fn = function() {
			var future = new Future;
			var args = Array.prototype.slice.call(arguments);
			if (multi) {
				var cb = future.resolver();
				args.push(function(err) {
					cb(err, Array.prototype.slice.call(arguments, 1));
				});
			} else {
				args.push(future.resolver());
			}
			future._ = fnOrObject.apply(this, args);
			return future;
		}
		// Modules like `request` return a function that has more functions as properties. Handle this
		// in some kind of reasonable way.
		if (!stop) {
			var proto = Object.create(fnOrObject);
			for (var ii in fnOrObject) {
				if (fnOrObject.hasOwnProperty(ii) && fnOrObject[ii] instanceof Function) {
					proto[ii] = proto[ii];
				}
			}
			fn.__proto__ = Future.wrap(proto, multi, suffix, true);
		}
		return fn;
	}
};

/**
 * Creates a future returning the given result value.
 */
Future.fromResult = function(value) {
	var future = new Future();
	future.return(value);
	return future;
}

Future.assertNoFutureLeftBehind = function() {
	var cachedFiberFuture = getCachedFiberFuture();
	if (cachedFiberFuture && cachedFiberFuture.activeFutures.length) {
		var message = ["There are outstanding futures. Construction call stacks:"];
		for (var i = 0; i < cachedFiberFuture.activeFutures.length; ++i) {
			message.push("#" + (i+1).toString());
			var stack = cachedFiberFuture.activeFuturesCtorStacks[i].split("\n");
			stack.shift();
			while (stack[0] && stack[0].indexOf("future.js") !== -1)
				stack.shift();
			message.push(stack.join("\n"));
		}
		throw new Error(message.join("\n"));
	}
}

function getFutures(args, removeResolved) {
	var futures = [];
	for (var ii = 0; ii < args.length; ++ii) {
		var arg = args[ii];
		if (arg instanceof Future) {
			// Ignore already resolved fibers
			if (removeResolved && arg.isResolved()) {
				continue;
			}
			futures.push(arg);
		} else if (arg instanceof Array) {
			for (var jj = 0; jj < arg.length; ++jj) {
				var aarg = arg[jj];
				if (aarg instanceof Future) {
					// Ignore already resolved fibers
					if (removeResolved && aarg.isResolved()) {
						continue;
					}
					futures.push(aarg);
				} else {
					throw new Error(aarg+ ' is not a future');
				}
			}
		} else {
			throw new Error(arg+ ' is not a future');
		}
	}
	return futures;
}

/**
 * Wait on a series of futures and then return. If the futures throw an exception this function
 * /won't/ throw it back. You can get the value of the future by calling get() on it directly. If
 * you want to wait on a single future you're better off calling future.wait() on the instance.
 */
Future.settle = function settle(/* ... */) {
	var futures = getFutures(arguments, true);

	// pull out a FiberFuture for reuse if possible
	var singleFiberFuture;
	for (var i = 0; i < futures.length; ++i) {
		var candidateFuture = futures[i];
		if (candidateFuture instanceof FiberFuture && !candidateFuture.started) {
			singleFiberFuture = candidateFuture;
			futures.splice(i, 1);
			break;
		}
	}

	// Resumes current fiber
	var fiber = Fiber.current;
	if (!fiber) {
		throw new Error('Can\'t wait without a fiber');
	}

	// Resolve all futures
	var pending = futures.length + (singleFiberFuture ? 1 : 0);
	function cb() {
		if (!--pending) {
			fiber.run();
		}
	}
	for (var ii = 0; ii < futures.length; ++ii) {
		futures[ii].resolve(cb, undefined, true);
	}

	// Reusing a fiber?
	if (singleFiberFuture) {
		singleFiberFuture.started = true;
		try {
			singleFiberFuture.return(
				singleFiberFuture.fn.apply(singleFiberFuture.context, singleFiberFuture.args));
		} catch(e) {
			singleFiberFuture.throw(e);
		}
		--pending;
	}

	// Yield this fiber
	if (pending) {
		Fiber.yield();
	}

	if (singleFiberFuture) {
		futures.push(singleFiberFuture);
	}
	return futures;
};

Future.wait = function wait() {
	Future.settle.apply(null, arguments)
	var futures = getFutures(arguments, false);
	var errors;

	for (var i = 0; i < futures.length; ++i) {
		var settled = futures[i];
		deleteActiveFuture(settled);
		if (settled.resolved && settled.error) {
			(errors || (errors = [])).push(settled.error);
		}
	}

	if (errors) {
		if (errors.length === 1) {
			throw errors[0];
		} else {
			var error = new Error();
			var message = ["Multiple exceptions were thrown:"];
			var stack = ["Multiple exceptions were thrown:"];
			for (var ei = 0; ei < errors.length; ++ei) {
				var err = errors[ei];
				message.push(err.message);
				stack.push(err.stack ? err.stack : err);
			}

			error.innerErrors = errors;
			error.message = message.join("\n");
			error.stack = stack.join("\n\n");

			throw error;
		}
	}
};

Future.prototype = {
	/**
	 * Return the value of this future. If the future hasn't resolved yet this will throw an error.
	 */
	get: function() {
		deleteActiveFuture(this);
		if (!this.resolved) {
			throw new Error('Future must resolve before value is ready');
		} else if (this.error) {
			// Link the stack traces up
			var error = this.error;
			var localStack = {};
			Error.captureStackTrace(localStack, Future.prototype.get);
			var futureStack = Object.getOwnPropertyDescriptor(error, 'futureStack');
			if (!futureStack) {
				futureStack = Object.getOwnPropertyDescriptor(error, 'stack');
				if (futureStack) {
					Object.defineProperty(error, 'futureStack', futureStack);
				}
			}
			if (futureStack && futureStack.get) {
				Object.defineProperty(error, 'stack', {
					get: function() {
						var stack = futureStack.get.apply(error);
						if (stack) {
							stack = stack.split('\n');
							return [stack[0]]
								.concat(localStack.stack.split('\n').slice(1))
								.concat('    - - - - -')
								.concat(stack.slice(1))
								.join('\n');
						} else {
							return localStack.stack;
						}
					},
					set: function(stack) {
						Object.defineProperty(error, 'stack', {
							value: stack,
							configurable: true,
							enumerable: false,
							writable: true,
						});
					},
					configurable: true,
					enumerable: false,
				});
			}
			throw error;
		} else {
			return this.value;
		}
	},

	/**
	 * Mark this future as returned. All pending callbacks will be invoked immediately.
	 */
	"return": function(value) {
		if (this.resolved) {
			throw new Error('Future resolved more than once');
		}
		this.value = value;
		this.resolved = true;

		var callbacks = this.callbacks;
		if (callbacks) {
			delete this.callbacks;
			for (var ii = 0; ii < callbacks.length; ++ii) {
				try {
					var ref = callbacks[ii];
					if (ref[1]) {
						ref[1](value);
					} else {
						ref[0](undefined, value);
					}
				} catch(ex) {
					// console.log('Resolve cb threw', String(ex.stack || ex.message || ex));
					process.nextTick(function() {
						throw(ex);
					});
				}
			}
		}
	},

	/**
	 * Throw from this future as returned. All pending callbacks will be invoked immediately.
	 */
	"throw": function(error) {
		if (this.resolved) {
			throw new Error('Future resolved more than once');
		} else if (!error) {
			throw new Error('Must throw non-empty error');
		}
		this.error = error;
		this.resolved = true;

		var callbacks = this.callbacks;
		if (callbacks) {
			delete this.callbacks;
			for (var ii = 0; ii < callbacks.length; ++ii) {
				try {
					var ref = callbacks[ii];
					if (ref[1]) {
						ref[0].throw(error);
					} else {
						ref[0](error);
					}
				} catch(ex) {
					// console.log('Resolve cb threw', String(ex.stack || ex.message || ex));
					process.nextTick(function() {
						throw(ex);
					});
				}
			}
		}
	},

	/**
	 * "detach" this future. Basically this is useful if you want to run a task in a future, you
	 * aren't interested in its return value, but if it throws you don't want the exception to be
	 * lost. If this fiber throws, an exception will be thrown to the event loop and node will
	 * probably fall down.
	 */
	detach: function() {
		this.resolve(function(err) {
			if (err) {
				throw err;
			}
		});
	},

	/**
	 * Returns whether or not this future has resolved yet.
	 */
	isResolved: function() {
		return this.resolved === true;
	},

	/**
	 * Returns a node-style function which will mark this future as resolved when called.
	 */
	resolver: function() {
		return function(err, val) {
			if (err) {
				this.throw(err);
			} else {
				this.return(val);
			}
		}.bind(this);
	},

	/**
	 * Waits for this future to resolve and then invokes a callback.
	 *
	 * If two arguments are passed, the first argument is a future which will be thrown to in the case
	 * of error, and the second is a function(val){} callback.
	 *
	 * If only one argument is passed it is a standard function(err, val){} callback.
	 */
	resolve: function(arg1, arg2, noDeactivate) {
		if (!noDeactivate) {
			deleteActiveFuture(this);
		}

		if (this.resolved) {
			if (arg2) {
				if (this.error) {
					arg1.throw(this.error);
				} else {
					arg2(this.value);
				}
			} else {
				arg1(this.error, this.value);
			}
		} else {
			(this.callbacks = this.callbacks || []).push([arg1, arg2]);
		}
		return this;
	},

	/**
	 * Resolve only in the case of success
	 */
	resolveSuccess: function(cb) {
		this.resolve(function(err, val) {
			if (err) {
				return;
			}
			cb(val);
		});
		return this;
	},

	/**
	 * Propogate results to another future.
	 */
	proxy: function(future) {
		this.resolve(function(err, val) {
			if (err) {
				future.throw(err);
			} else {
				future.return(val);
			}
		});
	},

	/**
	 * Propogate only errors to an another future or array of futures.
	 */
	proxyErrors: function(futures) {
		this.resolve(function(err) {
			if (!err) {
				return;
			}
			if (futures instanceof Array) {
				for (var ii = 0; ii < futures.length; ++ii) {
					futures[ii].throw(err);
				}
			} else {
				futures.throw(err);
			}
		});
		return this;
	},

	/**
	 * Waits for the future to settle. If the future throws an error, then wait() will rethrow that error.
	 */
	wait: function() {
		if (this.isResolved()) {
			return this.get();
		}
		Future.settle(this);
		return this.get();
	},
};

/**
 * A function call which loads inside a fiber automatically and returns a future.
 */
function FiberFuture(fn, context, args) {
	this.fn = fn;
	this.context = context;
	this.args = args;
	this.started = false;
	addActiveFuture(this);
	var that = this;
	process.nextTick(function() {
		if (!that.started) {
			that.started = true;
			Fiber(function() {
				try {
					that.return(fn.apply(context, args));
				} catch(e) {
					that.throw(e);
				}
			}).run();
		}
	});
}
util.inherits(FiberFuture, Future);
