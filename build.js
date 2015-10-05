#!/usr/bin/env node
var cp = require('child_process'),
	fs = require('fs'),
	path = require('path');

// Parse args
var force = false, debug = false;
var
	arch = process.arch,
	platform = process.platform,
	v8 = /[0-9]+\.[0-9]+/.exec(process.versions.v8)[0];
var args = process.argv.slice(2).filter(function(arg) {
	if (arg === '-f') {
		force = true;
		return false;
	} else if (arg.substring(0, 13) === '--target_arch') {
		arch = arg.substring(14);
	} else if (arg === '--debug') {
		debug = true;
	}
	return true;
});
if (!debug) {
	args.push('--release');
}
if (!{ia32: true, x64: true, arm: true, ppc: true, ppc64: true, s390: true, s390x: true}.hasOwnProperty(arch)) {
	console.error('Unsupported (?) architecture: `'+ arch+ '`');
	process.exit(1);
}

// Test for pre-built library
var modPath = platform+ '-'+ arch+ '-v8-'+ v8;
if (!force) {
	try {
		fs.statSync(path.join(__dirname, 'bin', modPath, 'fibers.node'));
		console.log('`'+ modPath+ '` exists; testing');
		cp.execFile(process.execPath, ['quick-test'], function(err, stdout, stderr) {
			if (err || stdout !== 'pass' || stderr) {
				console.log('Problem with the binary; manual build incoming');
				build();
			} else {
				console.log('Binary is fine; exiting');
			}
		});
	} catch (ex) {
		// Stat failed
		build();
	}
} else {
	build();
}

// Build it
function build() {
	cp.spawn(
		process.platform === 'win32' ? 'node-gyp.cmd' : 'node-gyp',
		['rebuild'].concat(args),
		{stdio: [process.stdin, process.stdout, process.stderr]})
	.on('exit', function(err) {
		if (err) {
			if (err === 127) {
				console.error(
					'node-gyp not found! Please upgrade your install of npm! You need at least 1.1.5 (I think) '+
					'and preferably 1.1.30.'
				);
			} else {
				console.error('Build failed');
			}
			return process.exit(err);
		}
		afterBuild();
	});
}

// Move it to expected location
function afterBuild() {
	var targetPath = path.join(__dirname, 'build', debug ? 'Debug' : 'Release', 'fibers.node');
	var installPath = path.join(__dirname, 'bin', modPath, 'fibers.node');

	try {
		fs.mkdirSync(path.join(__dirname, 'bin', modPath));
	} catch (ex) {}

	try {
		fs.statSync(targetPath);
	} catch (ex) {
		console.error('Build succeeded but target not found');
		process.exit(1);
	}
	fs.renameSync(targetPath, installPath);
	console.log('Installed in `'+ installPath+ '`');
}
