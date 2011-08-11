// gh-12
require('fibers');
Fiber(function() {
	if (!Fiber.current.started) {
		throw new Error;
	}
}).run();
console.log('pass');
