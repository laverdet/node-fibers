// gh-10
require('fibers');

Fiber(function() {
	process.title = 'pass';
}).run();
// sunos process.title doesn't work, regardless of fibers
console.log(process.platform === 'sunos' ? 'pass' : process.title);
