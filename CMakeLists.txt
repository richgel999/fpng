project(fpng_test)

cmake_minimum_required(VERSION 3.0)
option(BUILD_X64 "build 64-bit" TRUE)
option(SSE "SSE 4.1 support" FALSE)

message("Initial BUILD_X64=${BUILD_X64}")
message("Initial SSE=${SSE}")

if( NOT CMAKE_BUILD_TYPE )
  set( CMAKE_BUILD_TYPE Release )
endif()

message( ${PROJECT_NAME} " build type: " ${CMAKE_BUILD_TYPE} )

if (BUILD_X64)
	message("Building 64-bit")
else()
	message("Building 32-bit")
endif()

if (SSE)
	message("SSE enabled")
else()
	message("SSE disabled")
endif()

if (NOT MSVC)
   set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
   set(CMAKE_C_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

   set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
   set(CMAKE_C_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

   set(CMAKE_CXX_FLAGS -std=c++11)
   set(GCC_COMPILE_FLAGS "-fvisibility=hidden -fPIC -fno-strict-aliasing -D_LARGEFILE64_SOURCE=1 -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Isrc")

   if (NOT BUILD_X64)
	  set(GCC_COMPILE_FLAGS "${GCC_COMPILE_FLAGS} -m32")
   endif()

   set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS}")
   set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS}")

   set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} ${GCC_LINK_FLAGS} -Wl,-rpath .")

   set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} ${GCC_COMPILE_FLAGS}")
   set(CMAKE_C_FLAGS_RELEASE  "${CMAKE_C_FLAGS_RELEASE} ${GCC_COMPILE_FLAGS}")
   set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${GCC_COMPILE_FLAGS} -D_DEBUG")

   set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} ${GCC_COMPILE_FLAGS}")
   set(CMAKE_CXX_FLAGS_RELEASE  "${CMAKE_CXX_FLAGS_RELEASE} ${GCC_COMPILE_FLAGS}")
   set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${GCC_COMPILE_FLAGS} -D_DEBUG")
   
   if (SSE)
		set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -DFPNG_NO_SSE=0 -msse4.1 -mpclmul")
		set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -DFPNG_NO_SSE=0 -msse4.1 -mpclmul")
   else()
	  	set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -DFPNG_NO_SSE=1")
		set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -DFPNG_NO_SSE=1")
   endif()
else()
	set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS}")
	set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS}")
endif()

set(FPNG_SRC_LIST ${COMMON_SRC_LIST} 
	src/fpng.cpp
	src/fpng_test.cpp
	src/lodepng.cpp
	)

if (APPLE)
   set(BIN_DIRECTORY "bin_osx")
else()
   set(BIN_DIRECTORY "bin")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${BIN_DIRECTORY})

add_executable(fpng_test ${FPNG_SRC_LIST})

if (NOT MSVC)
   target_link_libraries(fpng_test m pthread)
endif()

install(TARGETS fpng_test DESTINATION bin)
