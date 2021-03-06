FIND_PATH(PLUTO_INCLUDE_DIR pluto/libpluto.h)

FIND_LIBRARY(PLUTO_LIBRARY NAMES pluto)

IF (PLUTO_INCLUDE_DIR AND PLUTO_LIBRARY)
  SET(PLUTO_FOUND TRUE)
ENDIF (PLUTO_INCLUDE_DIR AND PLUTO_LIBRARY)


IF (PLUTO_FOUND)
  IF (NOT PLUTO_FIND_QUIETLY)
    MESSAGE(STATUS "Found Pluto: ${PLUTO_LIBRARY}")
  ENDIF (NOT PLUTO_FIND_QUIETLY)
ELSE (PLUTO_FOUND)
  IF (PLUTO_FIND_REQUIRED)
    MESSAGE(FATAL_ERROR "Could not find Pluto")
  ENDIF (PLUTO_FIND_REQUIRED)
ENDIF (PLUTO_FOUND)

