// gh-1
var Fiber = require('fibers');

Fiber(function() {
	require('child_process').exec('echo pass', function(err, stdout) {
		if (err) console.log(err);
		require('util').print(stdout);
	});
}).run();
