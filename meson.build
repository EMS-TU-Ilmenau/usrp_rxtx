project('usrp_rxtx', 'cpp',
        default_options : ['cpp_std=c++20', 'warning_level=3',
                           'buildtype=release', 'b_ndebug=if-release'])
add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_GNU_SOURCE',
                      language : 'cpp')

gen_version = custom_target('gen-version',
                            command : [find_program('src/version.sh'), '@SOURCE_ROOT@', '@OUTPUT0@'],
                            depend_files : ['.git/HEAD', '.git/index'],
                            output : ['version.cpp'])

dep_rt = meson.get_compiler('cpp').find_library('rt')
dep_uhd = dependency('uhd')
dep_thread = dependency('threads')

usrp_rxtx = executable('usrp_rxtx', 'src/main.cpp', gen_version,
                        dependencies : [dep_rt, dep_thread, dep_uhd])


# define unit tests
tests = [
    ['test_dummy', 'test/test_dummy.cpp', []],
]

# run all unit tests
foreach test : tests
    exe = executable(test[0], test[1])
    test(test[0], exe, args : test[2])
endforeach
