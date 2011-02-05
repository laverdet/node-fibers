var fs = require('fs');

if (fs.statSync(process.execPath).mtime >
    fs.statSync(__dirname + '/src/fibers').mtime) {
  throw new Error(
    '`node` has a newer mtime than `fiber`; it is possible your build is out of date. This ' +
    'could happen if you upgrade node. Try `npm rebuild fibers` to rebuild. If that doesn\'t ' +
    'work you could consider running `touch ' + __dirname + 'src/fibers` and maybe there won\'t ' +
    'be a problem.');
} else if (!process.env.FIBER_SHIM) {
  throw new Error(
    'Fiber support was not enabled when you ran node. To enable support for fibers, please run ' +
    'node with the included `node-fibers` script. For example, instead of running:\n\n' +
    '  node script.js\n\n' +
    'You should run:\n\n' +
    '  node-fibers script.js\n\n' +
    'You will not be able to use Fiber without this support enabled.');
}

require('./src/fibers');
