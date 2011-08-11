require('fibers');
Fiber(function() {
	console.log('pass');
	process.exit();
}).run();
console.log('fail');
