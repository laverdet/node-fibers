// gh-16
require('fibers');
Fiber(function() {
	Fiber(function() {
		Fiber(function() {}).run();
	}).run();
}).run();
console.log('pass');
