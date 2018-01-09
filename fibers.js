if (process.fiberLib) {
	module.exports = process.fiberLib;
} else {
	var fs = require('fs'), path = require('path');

	// Seed random numbers [gh-82]
	Math.random();

	// Look for binary for this platform
	var modPath = path.join(__dirname, 'bin', process.platform+ '-'+ process.arch+ '-'+ process.versions.modules, 'fibers');
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

function setupAsyncHacks(Fiber) {
	// Older (or newer?) versions of node may not support this API
	try {
		var aw = process.binding('async_wrap');
		var kExecutionAsyncId = aw.constants.kExecutionAsyncId;
		var kTriggerAsyncId = aw.constants.kTriggerAsyncId;
		if (!aw.popAsyncIds || !aw.pushAsyncIds) {
			throw new Error;
		}
	} catch (err) {
		return;
	}

	function getAndClearStack() {
		var ii = aw.asyncIdStackSize();
		var stack = new Array(ii);
		for (; ii > 0; --ii) {
			var asyncId = aw.async_id_fields[kExecutionAsyncId];
			stack[ii - 1] = {
				asyncId: asyncId,
				triggerId: aw.async_id_fields[kTriggerAsyncId],
			};
			aw.popAsyncIds(asyncId);
		}
		return stack;
	}

	function restoreStack(stack) {
		for (var ii = 0; ii < stack.length; ++ii) {
			aw.pushAsyncIds(stack[ii].asyncId, stack[ii].triggerId);
		}
	}

	function wrapFunction(fn) {
		return function() {
			let stack = getAndClearStack();
			try {
				return fn.apply(this, arguments);
			} finally {
				restoreStack(stack);
			}
		}
	}

	// Monkey patch methods which may long jump
	Fiber.yield = wrapFunction(Fiber.yield);
	Fiber.prototype.run = wrapFunction(Fiber.prototype.run);
	Fiber.prototype.throwInto = wrapFunction(Fiber.prototype.throwInto);
}
