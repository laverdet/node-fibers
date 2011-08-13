var fs = require('fs'), path = require('path');

if (fs.statSync(process.execPath).mtime >
		fs.statSync(require.resolve('./src/fibers')).mtime) {
	throw new Error(
		'`node` has a newer mtime than `fiber`; it is possible your build is out of date. This ' +
		'could happen if you upgrade node. Try `npm rebuild fibers` to rebuild. If that doesn\'t ' +
		'work you could consider running `touch ' + __dirname + 'src/fibers` and maybe there won\'t ' +
		'be a problem.');
}

// Injects `Fiber` and `yield` in to global
require('./src/fibers');
