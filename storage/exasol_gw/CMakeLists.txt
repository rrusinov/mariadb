OPTION(WITH_EXASOL_GW_STORAGE_ENGINE
  "Build the EXASOL Session Gateway proxy storage engine plugin (ha_exasol_gw.so)" OFF)

IF(NOT WITH_EXASOL_GW_STORAGE_ENGINE)
  SET(PLUGIN_exasol_gw NO CACHE STRING "How to build plugin exasol_gw" FORCE)
  MESSAGE(STATUS "Skipping EXASOL proxy storage engine (enable with -DWITH_EXASOL_GW_STORAGE_ENGINE=ON)")
  RETURN()
ENDIF()

MESSAGE(STATUS "Building EXASOL Session Gateway proxy storage engine")

SET(EXASOL_GW_SOURCES
  exasol_arrow_ipc.cc
  exasol_native_write_batch.cc
  exasol_gw_pushdown.cc
  exasol_gw_sql_generator.cc
  exasol_gw_session.cc
  exasol_sessiongw_client.cc
  ha_exasol_gw.cc)

MYSQL_ADD_PLUGIN(exasol_gw ${EXASOL_GW_SOURCES} STORAGE_ENGINE MODULE_ONLY
  LINK_LIBRARIES ${SSL_LIBRARIES})

TARGET_INCLUDE_DIRECTORIES(exasol_gw PRIVATE ${SSL_INCLUDE_DIRS})
TARGET_COMPILE_DEFINITIONS(exasol_gw PRIVATE ${SSL_DEFINES})
