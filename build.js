#!/usr/bin/env node
var cp = require('child_process'),
	fs = require('fs'),
	path = require('path'),
	downloadNode = require('./download_node.js');

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
				build(function(error, result) {
					BuildElectron();
				});
			} else {
				console.log('Binary is fine; exiting');
			}
		});
	} catch (ex) {
		// Stat failed
		build(function(error, result) {
			BuildElectron();
		});
	}
} else {
	build(function(error, result) {
		BuildElectron();
	});
}

// Build it
function build(callback) {
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
			
			if (callback)
				callback('Build Failed', null)

			return process.exit(err);
		}
		afterBuild(modPath);
		if (callback)
			callback(null, true)
	});
}

// Move it to expected location
function afterBuild(deployPath) {
	var targetPath = path.join(__dirname, 'build', debug ? 'Debug' : 'Release', 'fibers.node');
	var installPath = path.join(__dirname, 'bin', deployPath, 'fibers.node');

	try {
		fs.mkdirSync(path.join(__dirname, 'bin', deployPath));
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


/************** Build for Electron **************
* 												*
* 												*
************************************************/
// For new versions of node and/or electron, add new elements to this array.
var electrons = [{target: '0.33.6', nodeVersion: '4.1.1', disturl: 'https://atom.io/download/atom-shell', v8: '4.5'}];

function BuildElectron() {
	var curIndex = 0;

	for (var i = 0; i < electrons.length; i++) {
		processElectron(electrons[i]);
	}
};

function processElectron(electron, callback) {
	var nodeExec = path.join(__dirname, 'node', electron.nodeVersion + ' - ' + arch, 'node.exe');
	var nodegyp = path.join(process.env.APPDATA || (process.platform == 'darwin' ? process.env.HOME + 'Library/Preference' : '/var/local'), "/npm/node_modules/pangyp/bin/node-gyp.js");
	var args = [nodegyp, 'configure', 'build', '--target=' + electron.target, '--arch=' + arch, '--dist-url=' + electron.disturl, '--msvs_version=2013', '-release'];
	var deployPath = platform + '-' + arch + '-v8-' + electron.v8;

	try {
		fs.statSync(nodeExec);
		console.log('\x1b[32m node v ' + electron.nodeVersion + ' allready exsists. Continuing to build \x1b[0m');
		electronBuild(nodeExec, args, deployPath);
	} catch (error) {
		fs.mkdirSync(path.dirname(nodeExec));
		downloadNode.download(electron.nodeVersion, nodeExec, function(error, result) {
			if (result) {
				electronBuild(result, args, deployPath);
			} else {
				throw error;
			};
		});
	};
}

function electronBuild(nodeExec, args, deployPath) {
	cp.spawn(
		nodeExec,
		args,
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
		afterBuild(deployPath);
	});
}
