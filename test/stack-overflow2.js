var Fiber = require('fibers');
Fiber(function() {
	// Because of how v8 handles strings, the call to new RegExp chews up a lot of stack space
	// outside of JS.
	function fn() {
		var foo = '';
		for (var ii = 0; ii < 1024; ++ii) {
			foo += 'a';
		}
		new RegExp(foo, 'g');
	}

	// Calculate how far we can go recurse without hitting the JS stack limit
	var max = 0;
	function testRecursion(ii) {
		++max;
		testRecursion(ii + 1);
	}
	try {
		testRecursion();
	} catch (err) {}

	// Recurse to the limit and then invoke a stack-heavy C++ operation
	function wasteStack(ii) {
		ii ? wasteStack(ii - 1) : fn();
	}
	wasteStack(max - 94);
	console.log('pass');
}).run();
