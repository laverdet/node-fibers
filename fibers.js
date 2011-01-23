if (process.env.FIBER_SHIM) {
  require('./src/fibers');
  return;
}

/**
 * If the library wasn't preloaded then we should gracefully fail instead of segfaulting if they
 * attempt to use a Fiber.
 */
Fiber = function() {
  throw new Error(
    'Fiber support was not enabled when you ran node. To enable support for fibers, please run ' +
    'node with the included `node-fibers` script. For example, instead of running:\n\n' +
    '  node script.js\n\n' +
    'You should run:\n\n' +
    '  node-fibers script.js\n\n' +
    'You will not be able to use Fiber without this support enabled.');
}
