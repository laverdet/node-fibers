#!/usr/bin/env node
var spawn = require('child_process').spawn,
	fs = require('fs'),
	path = require('path');

// Parse args
var force = false;
var arch = process.arch;
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
var file = 'fibers-'+ process.platform+ '-'+ arch+ '.node';
if (!force) {
	try {
		fs.statSync(path.join(__dirname, 'bin', file));
		console.log('`'+ file+ '` exists; skipping build');
		return process.exit();
	} catch (ex) {}
}

// Build it
spawn('./node_modules/node-gyp/bin/node-gyp.js', ['rebuild'].concat(args), {customFds: [0, 1, 2]}).on('exit', function(err) {
	if (err) {
		console.error('Build failed');
		return process.exit(err);
	}
	afterBuild();
});

// Move it to expected location
function afterBuild() {
	var target = path.join(__dirname, 'build', 'Release', file);
	var install = path.join(__dirname, 'bin', file);
	try {
		fs.statSync(target);
	} catch (ex) {
		console.error('Build succeeded but `'+ target+ '` not found');
		process.exit(1);
	}
	fs.renameSync(target, install);
	console.log('Installed in `'+ install+ '`');
}
