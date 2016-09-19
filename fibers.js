if (process.fiberLib) {
	module.exports = process.fiberLib;
} else {
	var fs = require('fs'), path = require('path');

	// Seed random numbers [gh-82]
	Math.random();

	// Look for binary for this platform
	var modPath = path.join(__dirname, 'bin', process.platform+ '-'+ process.arch+ '-'+ process.versions.modules, 'fibers');
	try {
		fs.statSync(modPath+ '.node');
	} catch (ex) {
		// No binary!
		console.error(
			'## There is an issue with `node-fibers` ##\n'+
			'`'+ modPath+ '.node` is missing.\n\n'+
			'Try running this to fix the issue: '+ process.execPath+ ' '+ __dirname.replace(' ', '\\ ')+ '/build'
		);
		throw new Error('Missing binary. See message above.');
	}

	// Pull in fibers implementation
	process.fiberLib = module.exports = require(modPath).Fiber;
}
