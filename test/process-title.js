// gh-10
require('fibers');

var title = process.title;
Fiber(function() {
	process.title = 'pass';
}).run();
console.log(process.title === 'pass' || process.title === title ? 'pass' : 'fail');
