if (process.fiberLib) {
	return module.exports = process.fiberLib;
}
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

// Pull in fibers implementation
process.fiberLib = module.exports = require(modPath).Fiber;
