project('actor','c')

add_global_arguments('-O2', language : 'c')

src_files = ['main.c', 'workq.c']

thread_dep = dependency('threads')

executable('actor', sources : src_files ,dependencies : thread_dep)
