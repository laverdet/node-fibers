#!/usr/bin/env node
var spawn = require('child_process').spawn,
	fs = require('fs'),
	path = require('path');

// Parse args
var force = false;
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
	}
	return true;
});
if (!{ia32: true, x64: true, arm: true}.hasOwnProperty(arch)) {
	console.error('Unsupported (?) architecture: `'+ arch+ '`');
	process.exit(1);
}

// Test for pre-built library
var modPath = platform+ '-'+ arch+ '-v8-'+ v8;
if (!force) {
	try {
		fs.statSync(path.join(__dirname, 'bin', modPath, 'fibers.node'));
		console.log('`'+ modPath+ '` exists; skipping build');
		return process.exit();
	} catch (ex) {}
}

// Build it
spawn(
	'node-gyp',
	['rebuild'].concat(args),
	{customFds: [0, 1, 2]})
.on('exit', function(err) {
	if (err) {
		console.error('Build failed');
		return process.exit(err);
	}
	afterBuild();
});

// Move it to expected location
function afterBuild() {
	var targetPath = path.join(__dirname, 'build', 'Release', 'fibers.node');
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
