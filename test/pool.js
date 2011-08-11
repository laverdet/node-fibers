require('fibers');

var fibers = [];
for (var jj = 0; jj < 10; ++jj) {
	for (var ii = 0; ii < 200; ++ii) {
		var fn = Fiber(function() {
			yield();
		});
		fn.run();
		fibers.push(fn);
	}
	for (var ii = 0; ii < fibers.length; ++ii) {
		fibers[ii].run();
	}
}
console.log('pass');
