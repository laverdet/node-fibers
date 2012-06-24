{
	'targets': [
		{
			'target_name': 'fibers',
			'sources': [
				'src/fibers.cc',
				'src/coroutine.cc',
				'src/libcoro/coro.c',
				# Rebuild on header changes
				'src/coroutine.h',
				'src/libcoro/coro.h',
			],
			'cflags!': ['-ansi'],
			'conditions': [
				['OS == "win"',
					{'defines': ['USE_WINFIBER']},
				# else
					{
						'cflags': ['-Wno-deprecated-declarations'],
						'defines': ['USE_CORO'],
						'ldflags': ['-pthread'],
					}
				],
				['OS == "linux" or OS == "solaris" or OS == "freebsd"', {'defines': ['CORO_UCONTEXT']}],
				['OS == "mac"', {
					'defines': ['CORO_SJLJ'],
					 'xcode_settings': {
	           'GCC_OPTIMIZATION_LEVEL': '3',
						 'GCC_GENERATE_DEBUGGING_SYMBOLS': 'NO',
					 },
				}],
				['OS == "openbsd"', {'defines': ['CORO_ASM']}],
				['target_arch == "arm"',
					{
						# There's been problems getting real fibers working on arm
						'defines': ['CORO_PTHREAD'],
						'defines!': ['CORO_UCONTEXT', 'CORO_SJLJ', 'CORO_ASM'],
					},
				],
			],
		},
	],
}
