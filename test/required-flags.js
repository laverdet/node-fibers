// fibers must refuse to load under node flags it cannot run with
var spawnSync = require('child_process').spawnSync;
var path = require('path');
var major = parseInt(process.versions.node, 10);
var fibers = path.join(__dirname, '..', 'fibers.js');

function load(flags) {
	var env = {};
	for (var ii in process.env) {
		if (ii !== 'NODE_OPTIONS') env[ii] = process.env[ii];
	}
	var result = spawnSync(process.execPath, flags.concat(['--no-warnings', '-e', 'require('+ JSON.stringify(fibers)+ ')']), { env: env, encoding: 'utf8' });
	return { ok: result.status === 0, stderr: result.stderr };
}

function refuses(flags, hint) {
	var result = load(flags);
	if (result.ok || result.stderr.indexOf(hint) === -1) {
		throw new Error('expected node '+ flags.join(' ')+ ' to be refused with "'+ hint+ '", got: '+ result.stderr);
	}
}

if (major >= 24) {
	refuses([], 'add --expose-internals');
	refuses(['--expose-internals'], 'add --no-async-context-frame');
	refuses(['--no-async-context-frame', '--expose-internals', '--conservative-stack-scanning'], 'remove --conservative-stack-scanning');
	if (!load(['--no-async-context-frame', '--expose-internals']).ok) {
		throw new Error('expected fibers to load with its required flags');
	}
} else {
	if (!load([]).ok) {
		throw new Error('expected fibers to load with no flags');
	}
	if (major >= 22) {
		refuses(['--experimental-async-context-frame'], 'remove --experimental-async-context-frame');
	}
}
console.log('pass');
