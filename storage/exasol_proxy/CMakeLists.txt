OPTION(WITH_EXASOL_PROXY_STORAGE_ENGINE
  "Build the EXASOL proxy storage engine plugin (ha_exasol_proxy.so)" OFF)
SET(EXASOL_DB_SOURCE_DIR "" CACHE PATH
  "Path to the Exasol db source tree (must contain Server/include/mariadbstorageenginecoreabi.h)")
SET(EXASOL_DB_BUILD_DIR "" CACHE PATH
  "Path to the Exasol db build/install tree used with the EXASOL MariaDB integration")

SET(_exasol_proxy_source_candidates)
IF(EXASOL_DB_SOURCE_DIR)
  LIST(APPEND _exasol_proxy_source_candidates "${EXASOL_DB_SOURCE_DIR}")
ENDIF()
LIST(APPEND _exasol_proxy_source_candidates
  "${CMAKE_SOURCE_DIR}/../db"
  "${CMAKE_SOURCE_DIR}/..")

SET(EXASOL_DB_SOURCE_DIR_RESOLVED "")
SET(EXASOL_DB_INCLUDE_DIR "")
FOREACH(_exasol_candidate ${_exasol_proxy_source_candidates})
  IF(EXISTS "${_exasol_candidate}/Server/include/mariadbstorageenginecoreabi.h")
    SET(EXASOL_DB_SOURCE_DIR_RESOLVED "${_exasol_candidate}")
    SET(EXASOL_DB_INCLUDE_DIR "${_exasol_candidate}/Server/include")
    BREAK()
  ENDIF()
ENDFOREACH()

IF(NOT WITH_EXASOL_PROXY_STORAGE_ENGINE)
  SET(PLUGIN_exasol_proxy NO CACHE STRING "How to build plugin exasol_proxy" FORCE)
  MESSAGE(STATUS "Skipping EXASOL proxy storage engine (enable with -DWITH_EXASOL_PROXY_STORAGE_ENGINE=ON)")
  RETURN()
ENDIF()

IF(EXASOL_DB_BUILD_DIR AND NOT EXISTS "${EXASOL_DB_BUILD_DIR}")
  MESSAGE(FATAL_ERROR
    "WITH_EXASOL_PROXY_STORAGE_ENGINE=ON but EXASOL_DB_BUILD_DIR does not exist: ${EXASOL_DB_BUILD_DIR}")
ENDIF()

IF(NOT EXASOL_DB_INCLUDE_DIR)
  STRING(REPLACE ";" "\n  " _exasol_candidate_list "${_exasol_proxy_source_candidates}")
  MESSAGE(FATAL_ERROR
    "WITH_EXASOL_PROXY_STORAGE_ENGINE=ON requires EXASOL_DB_SOURCE_DIR to point to an Exasol db source tree.\n"
    "Expected header: Server/include/mariadbstorageenginecoreabi.h\n"
    "Tried:\n  ${_exasol_candidate_list}")
ENDIF()

MESSAGE(STATUS "Building EXASOL proxy storage engine from ${EXASOL_DB_SOURCE_DIR_RESOLVED}")
IF(EXASOL_DB_BUILD_DIR)
  MESSAGE(STATUS "Using EXASOL_DB_BUILD_DIR=${EXASOL_DB_BUILD_DIR}")
ENDIF()
INCLUDE_DIRECTORIES(BEFORE ${EXASOL_DB_INCLUDE_DIR})

SET(EXASOL_PROXY_SOURCES
  exasol_proxy_sql_generator.cc
  ha_exasol_proxy.cc)

MYSQL_ADD_PLUGIN(exasol_proxy ${EXASOL_PROXY_SOURCES} STORAGE_ENGINE MODULE_ONLY
  LINK_LIBRARIES ${CMAKE_DL_LIBS})
