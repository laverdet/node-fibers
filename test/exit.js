var Fiber = require('fibers');
if (!process.stdout.write('pass\n')) {
	process.stdout.on('drain', go);
} else {
	go();
}
function go() {
	Fiber(function() {
		process.exit();
	}).run();
	console.log('fail');
}
