project('usrp_rxtx', 'cpp',
        default_options : ['cpp_std=c++20', 'warning_level=3',
                           'buildtype=release', 'b_ndebug=if-release'])
add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_GNU_SOURCE',
                      language : 'cpp')

# find external deoendencies
dep_rt = meson.get_compiler('cpp').find_library('rt')
dep_uhd = dependency('uhd')
dep_thread = dependency('threads')

# get libmosquitto dependency from mosquitto subproject
mosquitto = subproject('mosquitto')
dep_libmosquitto = mosquitto.get_variable('dep_libmosquitto')

# define custom target for git version information compiled into executables
gen_version = custom_target('gen-version',
                            command : [find_program('src/version.sh'), '@SOURCE_ROOT@', '@OUTPUT0@'],
                            depend_files : ['.git/HEAD', '.git/index'],
                            output : ['version.cpp'])

# define main target
usrp_rxtx = executable('usrp_rxtx',
        'src/main.cpp',
        gen_version,
        'src/config.cpp',
        'src/logging.cpp',
        'src/mqtt.cpp',
        'src/sync.cpp',
        'src/ringbuf.cpp',
        'src/rx.cpp',
        'src/tx.cpp',
        'src/wr.cpp',
    dependencies : [dep_rt, dep_thread, dep_uhd, dep_libmosquitto]
)

# define unit tests
tests = [
    ['test_config', ['test/test_config.cpp', 'src/config.cpp'], [join_paths(meson.current_source_dir(), 'src/config.cfg')]],
    ['test_json', 'test/test_json.cpp', []],
    ['test_mpsc', 'test/test_mpsc.cpp', []],
    ['test_ringbuf', ['test/test_ringbuf.cpp', 'src/ringbuf.cpp'], []]
]

# run all unit tests
foreach test : tests
    exe = executable(test[0], test[1])
    test(test[0], exe, args : test[2])
endforeach
