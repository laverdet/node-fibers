// gh-10
require('fibers');

Fiber(function() {
	process.title = 'pass';
}).run();
console.log(process.title);
