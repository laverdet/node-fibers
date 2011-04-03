var fs = require('fs'), path = require('path');

if (fs.statSync(process.execPath).mtime >
		fs.statSync(require.resolve('./src/fibers')).mtime) {
	throw new Error(
		'`node` has a newer mtime than `fiber`; it is possible your build is out of date. This ' +
		'could happen if you upgrade node. Try `npm rebuild fibers` to rebuild. If that doesn\'t ' +
		'work you could consider running `touch ' + __dirname + 'src/fibers` and maybe there won\'t ' +
		'be a problem.');
} else if (!process.env.FIBER_SHIM) {
	throw new Error(
		'Fiber support was not enabled when you ran node. To enable support for fibers, please run ' +
		'node with the included `node-fibers` script. For example, instead of running:\n\n' +
		'  node script.js\n\n' +
		'You should run:\n\n' +
		'  node-fibers script.js\n\n' +
		'You will not be able to use Fiber without this support enabled.');
}

require('./src/fibers');

// Shim child_process.spawn to shim any spawned Node instances
var fibersRoot = path.dirname(require.resolve('./src/fibers'));
var cp = require('child_process');
cp.spawn = function(spawn) {
	return function(command, args, options) {
		if (command === process.execPath) {
			options = Object.create(options || {});
			options.env = Object.create(options.env || {});
			options.env.FIBER_SHIM = '1';
			if (process.platform === 'linux2') {
				options.env.LD_PRELOAD = fibersRoot + '/coroutine.so';
			} else if (process.platform === 'darwin') {
				options.env.DYLD_INSERT_LIBRARIES = fibersRoot + '/coroutine.dylib';
				options.env.DYLD_FORCE_FLAT_NAMESPACE = '1';
				options.env.DYLD_LIBRARY_PATH = fibersRoot;
			} else {
				throw new Error('Unknown platform!');
			}
		}
		return spawn(command, args, options);
	};
}(cp.spawn);
