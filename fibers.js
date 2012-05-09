var fs = require('fs'), path = require('path');

// Look for binary for this platform
var modPath = path.join(__dirname, 'bin', process.platform+ '-'+ process.arch, 'fibers');
try {
	fs.statSync(modPath+ '.node');
} catch (ex) {
	// No binary!
	throw new Error('`'+ modPath+ '.node` is missing. Try reinstalling `node-fibers`?');
}

// Injects `Fiber` and `yield` in to global scope (for now)
require(modPath);
module.exports = Fiber;
