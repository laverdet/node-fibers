if (process.fiberLib) {
	module.exports = process.fiberLib;
} else {
	var fs = require('fs'), path = require('path'), detectLibc = require('detect-libc');

	// Seed random numbers [gh-82]
	Math.random();

	// Look for binary for this platform
	var modPath = path.join(__dirname, 'bin', process.platform+ '-'+ process.arch+ '-'+ process.versions.modules+
		((process.platform === 'linux') ? '-'+ detectLibc.family : ''), 'fibers');
	try {
		// Pull in fibers implementation
		process.fiberLib = module.exports = require(modPath).Fiber;
	} catch (ex) {
		// No binary!
		console.error(
			'## There is an issue with `node-fibers` ##\n'+
			'`'+ modPath+ '.node` is missing.\n\n'+
			'Try running this to fix the issue: '+ process.execPath+ ' '+ __dirname.replace(' ', '\\ ')+ '/build'
		);
		console.error(ex.stack || ex.message || ex);
		throw new Error('Missing binary. See message above.');
	}

	setupAsyncHacks(module.exports);
}

// What a fiber switch does to v8 is only sound with some of node's defaults turned off. Starting
// without them corrupts the heap or mixes async context between fibers, so refuse to load.
function checkFlags() {
	var flags = process.execArgv.concat((process.env.NODE_OPTIONS || '').split(/\s+/));
	var major = parseInt(process.versions.node, 10);
	var problems = [];
	// A conservative scan only walks the running fiber's stack, so what a suspended fiber's stack
	// refers to could be moved or freed.
	['--conservative-stack-scanning', '--scavenger-conservative-object-pinning'].forEach(function(flag) {
		if (flags.indexOf(flag) !== -1) {
			problems.push('remove '+ flag);
		}
	});
	// AsyncContextFrame keeps the current async context in one isolate-wide slot, which v8 visits
	// once for every suspended fiber during garbage collection.
	var asyncContextFrame;
	if (major >= 24) {
		try {
			asyncContextFrame = require('internal/async_context_frame').enabled;
		} catch (err) {
			problems.push('add --expose-internals');
		}
	} else {
		asyncContextFrame = flags.indexOf('--experimental-async-context-frame') !== -1;
	}
	if (asyncContextFrame) {
		problems.push(major >= 24 ? 'add --no-async-context-frame' : 'remove --experimental-async-context-frame');
	}
	if (problems.length) {
		throw new Error('fibers cannot run with these node flags: '+ problems.join(', '));
	}
}

function setupAsyncHacks(Fiber) {
	checkFlags();
	// A failure here is not swallowed: without the async id stack swap below, a fiber switch leaves
	// async_hooks (and so AsyncLocalStorage) on the wrong context.
	{
		var aw;
		try {
			aw = process.binding('async_wrap');
		} catch (err) {
			// Node 24+ no longer exposes this through process.binding; needs --expose-internals
			aw = require('internal/test/binding').internalBinding('async_wrap');
		}
		var getAsyncIdStackSize;

		if (aw.asyncIdStackSize instanceof Function) {
			getAsyncIdStackSize = aw.asyncIdStackSize;
		} else if (aw.constants.kStackLength !== undefined) {
			getAsyncIdStackSize = function(kStackLength) {
				return function() {
					return aw.async_hook_fields[kStackLength];
				};
			}(aw.constants.kStackLength);
		} else {
			throw new Error('Couldn\'t figure out how to get async stack size');
		}

		var popAsyncContext = aw.popAsyncContext || aw.popAsyncIds;
		var pushAsyncContext = aw.pushAsyncContext || aw.pushAsyncIds;
		if (!popAsyncContext || !pushAsyncContext) {
			throw new Error('Push/pop do not exist');
		}

		var kExecutionAsyncId;
		if (aw.constants.kExecutionAsyncId === undefined) {
			kExecutionAsyncId = aw.constants.kCurrentAsyncId;
		} else {
			kExecutionAsyncId = aw.constants.kExecutionAsyncId;
		}
		var kTriggerAsyncId;
		if (aw.constants.kTriggerAsyncId === undefined) {
			kTriggerAsyncId = aw.constants.kCurrentTriggerId;
		} else {
			kTriggerAsyncId = aw.constants.kTriggerAsyncId;
		}

		var asyncIds = aw.async_id_fields || aw.async_uid_fields;

		function getAndClearStack() {
			var ii = getAsyncIdStackSize();
			var stack = new Array(ii);
			for (; ii > 0; --ii) {
				var asyncId = asyncIds[kExecutionAsyncId];
				stack[ii - 1] = {
					asyncId: asyncId,
					triggerId: asyncIds[kTriggerAsyncId],
				};
				popAsyncContext(asyncId);
			}
			return stack;
		}

		function restoreStack(stack) {
			for (var ii = 0; ii < stack.length; ++ii) {
				pushAsyncContext(stack[ii].asyncId, stack[ii].triggerId);
			}
		}

		function logUsingFibers(fibersMethod) {
			const logUseFibersLevel = +(process.env.ENABLE_LOG_USE_FIBERS || 0);

			if (!logUseFibersLevel) return;

			if (logUseFibersLevel === 1) {
				console.warn(`[FIBERS_LOG] Using ${fibersMethod}.`);
				return;
			}

			const { LOG_USE_FIBERS_INCLUDE_IN_PATH } = process.env;
			const stackFromError = new Error(`[FIBERS_LOG] Using ${fibersMethod}.`).stack;

			if (
				!LOG_USE_FIBERS_INCLUDE_IN_PATH ||
				stackFromError.includes(LOG_USE_FIBERS_INCLUDE_IN_PATH)
			) {
				console.warn(stackFromError);
			}
		}

		function wrapFunction(fn, fibersMethod) {
			return function () {
				logUsingFibers(fibersMethod);
				var stack = getAndClearStack();
				try {
					return fn.apply(this, arguments);
				} finally {
					restoreStack(stack);
				}
			};
		}

		// Monkey patch methods which may long jump
		Fiber.yield = wrapFunction(Fiber.yield, "Fiber.yield");
		Fiber.prototype.run = wrapFunction(Fiber.prototype.run, "Fiber.run");
		Fiber.prototype.throwInto = wrapFunction(
			Fiber.prototype.throwInto,
			"Fiber.throwInto"
		);

	}
}
