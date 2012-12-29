var fs = require('fs'), path = require('path');

// Seed random numbers [gh-82]
Math.random();

// Look for binary for this platform
var v8 = 'v8-'+ /[0-9]+\.[0-9]+/.exec(process.versions.v8)[0];
var modPath = path.join(__dirname, 'bin', process.platform+ '-'+ process.arch+ '-'+ v8, 'fibers');
try {
	fs.statSync(modPath+ '.node');
} catch (ex) {
	// No binary!
	throw new Error('`'+ modPath+ '.node` is missing. Try reinstalling `node-fibers`?');
}

// Injects `Fiber` and `yield` in to global scope (for now)
require(modPath);
module.exports = Fiber;

// Function.prototype.toSync - Wrap function as Sync
Object.defineProperty(Function.prototype,"toSync", {
	enumerable: false, configurable: false, 
	get: function() { 
		return (function(context){ 
			return (function(/*...*/) { 
				var a = []; for(var i in arguments) { a.push(arguments[i]); }
				return (require("fibers/future").wrap(context)).apply(context,a).wait(); 
			});
		})(this); },
	set: function() { }

});

// Function.prototype.toSync toString lock
Object.defineProperty(Function.prototype.toSync,"toString", {
	enumerable: false, configurable: false, get: function() { return function() {} }, set: function() { }
});

// Function.prototype.toFiber - Run function as Fiber
Object.defineProperty(Function.prototype,"asFiber", {
	enumerable: false, configurable: false, get: function() { return require("fibers")(this).run(); }, set: function() { }
});

// Function.prototype.toFiber toString lock
Object.defineProperty(Function.prototype.asFiber,"toString", {
	enumerable: false, configurable: false, get: function() { return function() {} }, set: function() { }
});
