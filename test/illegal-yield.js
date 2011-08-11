// gh-3
require('fibers');
try {
	yield();
} catch(err) {
	console.log('pass');
}
