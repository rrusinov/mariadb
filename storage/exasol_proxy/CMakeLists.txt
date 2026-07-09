OPTION(WITH_EXASOL_PROXY_STORAGE_ENGINE
  "Build the EXASOL Session Gateway proxy storage engine plugin (ha_exasol_proxy.so)" OFF)

IF(NOT WITH_EXASOL_PROXY_STORAGE_ENGINE)
  SET(PLUGIN_exasol_proxy NO CACHE STRING "How to build plugin exasol_proxy" FORCE)
  MESSAGE(STATUS "Skipping EXASOL proxy storage engine (enable with -DWITH_EXASOL_PROXY_STORAGE_ENGINE=ON)")
  RETURN()
ENDIF()

MESSAGE(STATUS "Building EXASOL Session Gateway proxy storage engine")

SET(EXASOL_PROXY_SOURCES
  exasol_arrow_ipc.cc
  exasol_proxy_pushdown.cc
  exasol_proxy_sql_generator.cc
  exasol_sessiongw_client.cc
  ha_exasol_proxy.cc)

MYSQL_ADD_PLUGIN(exasol_proxy ${EXASOL_PROXY_SOURCES} STORAGE_ENGINE MODULE_ONLY
  LINK_LIBRARIES ${SSL_LIBRARIES})

TARGET_INCLUDE_DIRECTORIES(exasol_proxy PRIVATE ${SSL_INCLUDE_DIRS})
TARGET_COMPILE_DEFINITIONS(exasol_proxy PRIVATE ${SSL_DEFINES})
