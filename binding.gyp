{
	'targets': [
		{
			'target_name': 'fibers',
			'sources': [
				'src/fibers.cc',
				'src/coroutine.cc',
				'src/libcoro/coro.c',
				# to help IDE experience
				'src/coroutine.h',
				'src/libcoro/coro.h',
			],

			'conditions': [
					['OS=="linux"', {
						'defines': [
							'CORO_UCONTEXT',
							'USE_CORO',
						],
					}],
					['OS=="win"', {
						'defines': [
							'USE_WINFIBER',
						],
					}, { # OS != "win",
					}]
				],
		},
	]
}
