CMAKE_MINIMUM_REQUIRED( VERSION 3.3 )

PROJECT( ${COMMON_LIB_NAME} )

FILE(GLOB SOURCES *.cpp)
file(GLOB HEADERS *.h)

ADD_LIBRARY( ${COMMON_LIB_NAME} ${SOURCES} ${HEADERS})
TARGET_LINK_LIBRARIES(${COMMON_LIB_NAME}
	${CRYPTO_LIB_NAME}
)
