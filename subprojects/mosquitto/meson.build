project('mosquitto', ['c', 'cpp'],
        version: '2.0.18',
        default_options: ['c_std=c11', 'cpp_std=c++11', 'warning_level=2',
                          'default_library=static',
                          'buildtype=release', 'b_ndebug=if-release'])

# configure features (see mosquitto/CMakeLists.txt and mosquitto/lib/CMakeLists.txt)
add_project_arguments('-DWITH_TLS', '-DWITH_THREADING', language: ['c', 'cpp'])

# mosquitto as of v2.0.18 uses the OpenSSL 1.1 ENGINE API, which is deprecated
# as of OpenSSL 3.0. define the OPENSSL_API_COMPAT version compatibility macro:
# https://www.openssl.org/docs/manmaster/man7/OPENSSL_API_COMPAT.html
# upstream pull request: https://github.com/eclipse/mosquitto/pull/3050
add_project_arguments('-DOPENSSL_API_COMPAT=0x10100000L', language: 'c')

# find external dependencies
dep_thread = dependency('threads')
dep_tls = dependency('openssl')

# configure include directories
inc_build = include_directories('mosquitto', 'mosquitto/deps', 'mosquitto/lib')
inc_cpp = include_directories('mosquitto/lib/cpp')
inc_link = include_directories('mosquitto/include')

libmosquitto = library('mosquitto',
		'mosquitto/lib/actions.c',
		'mosquitto/lib/alias_mosq.c',
		'mosquitto/lib/callbacks.c',
		'mosquitto/lib/connect.c',
		'mosquitto/lib/handle_auth.c',
		'mosquitto/lib/handle_connack.c',
		'mosquitto/lib/handle_disconnect.c',
		'mosquitto/lib/handle_ping.c',
		'mosquitto/lib/handle_pubackcomp.c',
		'mosquitto/lib/handle_publish.c',
		'mosquitto/lib/handle_pubrec.c',
		'mosquitto/lib/handle_pubrel.c',
		'mosquitto/lib/handle_suback.c',
		'mosquitto/lib/handle_unsuback.c',
		'mosquitto/lib/helpers.c',
		'mosquitto/lib/logging_mosq.c',
		'mosquitto/lib/loop.c',
		'mosquitto/lib/memory_mosq.c',
		'mosquitto/lib/messages_mosq.c',
		'mosquitto/lib/misc_mosq.c',
		'mosquitto/lib/mosquitto.c',
		'mosquitto/lib/net_mosq.c',
		'mosquitto/lib/net_mosq_ocsp.c',
		'mosquitto/lib/options.c',
		'mosquitto/lib/packet_datatypes.c',
		'mosquitto/lib/packet_mosq.c',
		'mosquitto/lib/property_mosq.c',
		'mosquitto/lib/read_handle.c',
		'mosquitto/lib/send_connect.c',
		'mosquitto/lib/send_disconnect.c',
		'mosquitto/lib/send_mosq.c',
		'mosquitto/lib/send_publish.c',
		'mosquitto/lib/send_subscribe.c',
		'mosquitto/lib/send_unsubscribe.c',
		'mosquitto/lib/socks_mosq.c',
		'mosquitto/lib/srv_mosq.c',
		'mosquitto/lib/strings_mosq.c',
		'mosquitto/lib/thread_mosq.c',
		'mosquitto/lib/time_mosq.c',
		'mosquitto/lib/tls_mosq.c',
		'mosquitto/lib/utf8_mosq.c',
		'mosquitto/lib/util_mosq.c',
		'mosquitto/lib/util_topic.c',
		'mosquitto/lib/will_mosq.c',
	include_directories: [inc_build, inc_link],
	dependencies: [dep_thread, dep_tls]
)
dep_libmosquitto = declare_dependency(link_with: libmosquitto, include_directories: inc_link)

libmosquittopp = library('mosquittopp',
		'mosquitto/lib/cpp/mosquittopp.cpp',
	include_directories: inc_cpp,
	dependencies: dep_libmosquitto
)
dep_libmosquittopp = declare_dependency(link_with: libmosquittopp, include_directories: inc_cpp)
