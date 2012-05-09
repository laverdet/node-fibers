#!/usr/bin/env node
var fs = require('fs');
var spawn = require('child_process').spawn;
var path = require('path');

function runTest(test, cb) {
	var proc = spawn(process.execPath, [path.join('test', test)], {env: {NODE_PATH: __dirname}});
	proc.stdout.setEncoding('utf8');
	proc.stderr.setEncoding('utf8');

	var stdout = '', stderr = '';
	proc.stdout.on('data', function(data) {
		stdout += data;
	});
	proc.stderr.on('data', function(data) {
		stderr += data;
	});
	proc.stdin.end();

	proc.on('exit', function(code) {
		if (stdout !== 'pass\n' || stderr !== '') {
			return cb(new Error(
					'Test `'+ test+ '` failed.\n'+
					'code: '+ code+ '\n'+
					'stderr: '+ stderr+ '\n'+
					'stdout: '+ stdout));
		}
		console.log(test+ ': '+ 'pass');
		cb();
	});
}

var cb = function(err) {
	if (err) {
		console.error(String(err));
		process.exit(1);
	}
};
fs.readdirSync('./test').reverse().forEach(function(file) {
	cb = new function(cb, file) {
		return function(err) {
			if (err) return cb(err);
			runTest(file, cb);
		};
	}(cb, file);
});
cb();
