include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${DRM_INCLUDE_DIRS}
  ${CURSES_INCLUDE_DIRS}
  )

file(GLOB XRT_USER_GEM_FILES
  "*.h"
  "*.cpp"
  "*.c"
  )

set(CMAKE_CXX_FLAGS "-DXCLHAL_MAJOR_VER=2 ${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "-DXCLHAL_MINOR_VER=1 ${CMAKE_CXX_FLAGS}")

#source_group(XRT_XOCL_API_FILES FILES ${XRT_XOCL_API_FILES})
#source_group(XRT_XOCL_CORE_FILES FILES ${XRT_XOCL_CORE_FILES})
#source_group(XRT_XOCL_XCLBIN_FILES FILES ${XRT_XOCL_XCLBIN_FILES})

set(XRT_CORE_SRC
  ${XRT_USER_GEM_FILES}
  )

add_library(user_gem OBJECT ${XRT_CORE_SRC})
