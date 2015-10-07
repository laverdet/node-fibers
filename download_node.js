var http = require('https');
var path = require('path');

module.exports = {
	download: function(version, destination, callback) {	
		var url = 'https://node.org/dist/v' + version + '/' 
			+ (process.platform === 'win32' ? '/win-x86/node.exe' : '/win-x64/node.exe');

		console.log('\x1b[32m Starting to download node v ' + version + ' \x1b[0m');	
		http.get(url, function (res) {
			if (res.statusCode !== 200 && callback) {
				callback('HTTP response status code ' + res.statusCode, null);
			};
		
			var destFile = path.join(destination);
		
			var stream = require('fs').createWriteStream(destFile);
			res.pipe(stream);
			
			stream.on('finish', function() {
				console.log('\x1b[32m Downloading node v ' + version + ' success \x1b[0m');
				callback(null, destFile);
    		});
		});
	}
};