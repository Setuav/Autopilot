set(PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_DIR})
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(TEMPLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/templates)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	${CMAKE_CURRENT_SOURCE_DIR}/proto/topics.yaml
)

execute_process(
	COMMAND
		${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_cpp.py
		--yaml-file ${CMAKE_CURRENT_SOURCE_DIR}/proto/topics.yaml
		--list-topics
	OUTPUT_VARIABLE PBLINK_TOPIC_LIST
	OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE PBLINK_TOPIC_LIST_RESULT
)

if(NOT PBLINK_TOPIC_LIST_RESULT EQUAL 0)
	message(FATAL_ERROR "Failed to read pblink topics from topics.yaml")
endif()

set(ALL_TOPICS ${PBLINK_TOPIC_LIST})

set(PROTO_FILES)
set(NANOPB_C_FILES)
set(NANOPB_H_FILES)

foreach(TOPIC ${ALL_TOPICS})
	list(APPEND PROTO_FILES ${PROTO_DIR}/${TOPIC}.proto)
	list(APPEND NANOPB_C_FILES ${GENERATED_DIR}/${TOPIC}.pb.c)
	list(APPEND NANOPB_H_FILES ${GENERATED_DIR}/${TOPIC}.pb.h)
endforeach()

add_custom_command(
	OUTPUT
		${GENERATED_DIR}/uorb_to_proto.h
		${GENERATED_DIR}/uorb_to_proto.cpp
		${GENERATED_DIR}/pblink_topics.h
		${GENERATED_DIR}/pblink_downlink.h
		${GENERATED_DIR}/pblink_uplink.h
		${PROTO_FILES}
	COMMAND
		${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_cpp.py
		--yaml-file ${CMAKE_CURRENT_SOURCE_DIR}/proto/topics.yaml
		--msg-dir ${PX4_SOURCE_DIR}/msg
		--template-dir ${TEMPLATE_DIR}
		--proto-dir ${PROTO_DIR}
		--generated-dir ${GENERATED_DIR}
	DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/generate_cpp.py
		${CMAKE_CURRENT_SOURCE_DIR}/proto/topics.yaml
		${TEMPLATE_DIR}/proto.j2
		${TEMPLATE_DIR}/converter_h.j2
		${TEMPLATE_DIR}/converter_cpp.j2
		${TEMPLATE_DIR}/topics_header.j2
		${TEMPLATE_DIR}/downlink_handlers.j2
		${TEMPLATE_DIR}/uplink_senders.j2
	COMMENT "Generating Protobuf and C++ converter files for pblink"
)

set(NANOPB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/nanopb)
set(NANOPB_GENERATOR ${NANOPB_DIR}/generator/nanopb_generator.py)

foreach(TOPIC ${ALL_TOPICS})
	add_custom_command(
		OUTPUT
			${GENERATED_DIR}/${TOPIC}.pb.c
			${GENERATED_DIR}/${TOPIC}.pb.h
		COMMAND
			${PYTHON_EXECUTABLE} ${NANOPB_GENERATOR}
			--output-dir=${GENERATED_DIR}
			-I${PROTO_DIR}
			-I${NANOPB_DIR}/generator
			${PROTO_DIR}/${TOPIC}.proto
		DEPENDS
			${PROTO_DIR}/${TOPIC}.proto
			${NANOPB_GENERATOR}
		COMMENT "Running nanopb generator for ${TOPIC}"
	)
endforeach()

add_custom_target(pblink_gen DEPENDS
	${GENERATED_DIR}/uorb_to_proto.h
	${NANOPB_H_FILES}
)

set(PBLINK_SRCS
	pblink.cpp
	${GENERATED_DIR}/uorb_to_proto.cpp
	${NANOPB_DIR}/pb_common.c
	${NANOPB_DIR}/pb_decode.c
	${NANOPB_DIR}/pb_encode.c
	${NANOPB_C_FILES}
)

px4_add_module(
	MODULE modules__pblink
	MAIN pblink
	SRCS ${PBLINK_SRCS}
	INCLUDES
		${CMAKE_CURRENT_BINARY_DIR}
		${GENERATED_DIR}
		${NANOPB_DIR}
	DEPENDS
		pblink_gen
		crc
	MODULE_CONFIG
		module.yaml
)

target_include_directories(modules__pblink
	PRIVATE
	${PX4_BINARY_DIR}/platforms/common/uORB
)

set(PBLINK_UDP_SUPPORTED_BY_TARGET FALSE)

if("${PX4_PLATFORM}" STREQUAL "posix")
	set(PBLINK_UDP_SUPPORTED_BY_TARGET TRUE)
elseif(CONFIG_NET AND CONFIG_NET_UDP)
	set(PBLINK_UDP_SUPPORTED_BY_TARGET TRUE)
endif()

if(PBLINK_UDP_SUPPORTED_BY_TARGET)
	target_compile_definitions(modules__pblink
		PRIVATE
		PBLINK_UDP_SUPPORTED
	)
	message(STATUS "Pblink: UDP support enabled (${PX4_PLATFORM})")
else()
	message(STATUS "Pblink: UDP support disabled (${PX4_PLATFORM})")
endif()
