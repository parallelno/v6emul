if(WIN32)
	add_executable(test_client WIN32 main.cpp)
	target_link_libraries(test_client PRIVATE v6ipc ws2_32)
	target_include_directories(test_client PRIVATE
		${CMAKE_SOURCE_DIR}/libs/v6core/include)
else()
	message(STATUS "test_client disabled: Windows-only target")
endif()
