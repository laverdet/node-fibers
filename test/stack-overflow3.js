var Fiber = require('fibers');

// Create a bunch of fibers and consume each fiber's stack, then yield
var fibers = [];
for (var ii = 0; ii < 100; ++ii) {
    var fiber;
    fibers.push(fiber = Fiber(function() {
        var caught = false;
        var recur = 0, stop;
        function foo() {
            ++recur;
            try {
                foo();
            } catch (err) {
                if (!caught) {
                    stop = recur - 500;
                    caught = true;
                } else if (stop === recur) {
                    process.stdout.write(''); // do a thing?
                    return Fiber.yield();
                }
                --recur;
                throw err;
            }
        }
        foo();
    }));
    fiber.run();
}

// Unwind started fibers
fibers.forEach(function(fiber) {
    fiber.run();
});

console.log('pass');
