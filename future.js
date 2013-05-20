// Modified version of fiber/future.js that changes the behavior of detach and provides a way to access orphaned future throws

"use strict";
var Fiber = require('./fibers');
var util = require('util');
module.exports = Future;
Function.prototype.future = function() {
    var fn = this;
    var ret = function() {
        return new FiberFuture(fn, this, arguments);
    };
    ret.toString = function() {
        return '<<Future '+ fn+ '.future()>>';
    };
    return ret;
};

function Future() {}

/**
 * Wrap a node-style async function to return a future in place of using a callback.
 */
Future.wrap = function(fn, idx) {
    idx = idx === undefined ? fn.length - 1 : idx;
    return function() {
        var args = Array.prototype.slice.call(arguments);
        if (args.length > idx) {
            throw new Error('function expects no more than '+ idx+ ' arguments');
        }
        var future = new Future;
        args[idx] = future.resolver();
        fn.apply(this, args);
        return future;
    };
};

/**
 * Wait on a series of futures and then return. If the futures throw an exception this function
 * /won't/ throw it back. You can get the value of the future by calling get() on it directly. If
 * you want to wait on a single future you're better off calling future.wait() on the instance.
 */
Future.wait = function wait(/* ... */) {

    // Normalize arguments + pull out a FiberFuture for reuse if possible
    var futures = [], singleFiberFuture;
    for (var ii = 0; ii < arguments.length; ++ii) {
        var handleArg = function(arg) {
            // Ignore already resolved fibers
            if (arg.isResolved()) {
                return true; // continue
            }
            // Look for fiber reuse
            if (!singleFiberFuture && arg instanceof FiberFuture && !arg.started) {
                singleFiberFuture = arg;
                return true; // continue
            }
            futures.push(arg);
        };

        var arg = arguments[ii];
        if (arg instanceof Future) {
            if(handleArg(arg)) continue;
        } else if (arg instanceof Array) {
            for (var jj = 0; jj < arg.length; ++jj) {
                var aarg = arg[jj];
                if (aarg instanceof Future) {
                    if(handleArg(aarg)) continue;
                } else {
                    throw new Error(aarg+ ' is not a future');
                }
            }
        } else {
            throw new Error(arg+ ' is not a future');
        }
    }

    // Resumes current fiber
    var fiber = Fiber.current;
    if (!fiber) {
        throw new Error("Can't wait without a fiber");
    }

    // Resolve all futures
    var pending = futures.length + (singleFiberFuture ? 1 : 0);
    function cb() {
        if (!--pending) {
            fiber.run();
        }
    }
    for (var ii = 0; ii < futures.length; ++ii) {
        futures[ii].resolve(cb);
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
};

/*private*/ Future.errors = []; // stores future.throws that haven't yet been seen (by using future.wait or future.detach)
/*private*/ Future.removeError = function(error) {
    // remove error
    var i = Future.errors.indexOf(error);
    if(i != -1) {
        Future.errors.splice(i, 1);
    }
};
Future.forgottenErrors = function(callback) {
    var futuresLength = Future.errors.length;
    for(var n=0; n<futuresLength; n++) {
        var error = Future.errors[n];
        callback(error);
    }

    Future.errors.splice(0,futuresLength);
}

Future.prototype = {
    /**
     * Return the value of this future. If the future hasn't resolved yet this will throw an error.
     */
    get: function() {
        if (!this.resolved) {
            throw new Error('Future must resolve before value is ready');
        } else if (this.error) {
            // Link the stack traces up
            var stack = {}, error = this.error instanceof Object ? this.error : new Error(this.error);
            Future.removeError(this.error);

            var longError = Object.create(error);
            Error.captureStackTrace(stack, Future.prototype.get);
            Object.defineProperty(longError, 'stack', {
                get: function() {
                    var baseStack = error.stack;
                    if (baseStack) {
                        baseStack = baseStack.split('\n');
                        return [baseStack[0]]
                                .concat(stack.stack.split('\n').slice(1))
                                .concat('    - - - - -')
                                .concat(baseStack.slice(1))
                                .join('\n');
                    } else {
                        return stack.stack;
                    }
                },
                enumerable: true,
            });

            throw longError;
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
                        throw(ex);      // why next tick? So all the callbacks have a chance to run without being blocked by erroneous callbacks?
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
        Future.errors.push(error);

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
                    process.nextTick(function() { // again why next tick?
                        throw(ex);
                    });
                }
            }
        }
    },

    /**
     * "detach" this future. Basically this is useful if you want to run a task in a future, you
     * aren't interested in its return value, but if it throws you don't want the exception to be
     * lost. If the future is already resolved with a throw, the error will be thrown from detach.
     * If the future is not resolved yet, but eventually resolves with a throw, the error will be
     * thrown from the future.throw call.
     *
     * Todo: make it so that detach *always* causes a throw from the future.throw call, rather than
     * sometimes doing one thing and sometimes doing another (non-deterministic code is bad). This
     * can be done by having the future.throw call wait until the future is waited on or detatched.
     * When the future is waited on, throw it from the wait in the usual way. When the future is
     * detached, stop waiting in the future.throw call and throw an exception (in the future.throw
     * call).
     */
    detach: function() {
        var me = this;
        this.resolve(function(err) {
            if (err) {
                Future.removeError(err);
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
     * If only one argument is passed it is a standard function(err, val){} callback.
     *
     * If two arguments are passed, the first argument is a future which will be thrown to in the case
     * of error, and the second is a function(val){} callback.
     */
    resolve: function(arg1, arg2) {
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

        if(this.error) Future.removeError(this.error);

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
     * Differs from its functional counterpart in that it actually resolves the future. Thus if the
     * future threw, future.wait() will throw.
     */
    wait: function() {
        if (this.isResolved()) {
            return this.get();
        }
        Future.wait(this);
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
