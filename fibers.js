if (process.fiberLib) {
	module.exports = process.fiberLib;
} else {

	// Find binary
	var preGyp = require('node-pre-gyp');
	var path = require('path');
	var bindingPath = preGyp.find(path.resolve(path.join(__dirname, './package.json')));

	// Seed random numbers [gh-82]
	Math.random();

	// Look for binary for this platform
	try {
		// Pull in fibers implementation
		process.fiberLib = module.exports = require(bindingPath).Fiber;
	} catch (ex) {
		// No binary!
		console.error(
			'## There is an issue with `node-fibers` ##\n'+
			'`'+ bindingPath+ '.node` is missing.\n\n'+
			'Try reinstalling this package.'
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

		function wrapFunction(fn) {
			return function() {
				var stack = getAndClearStack();
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

	} catch (err) {
		return;
	}
}
