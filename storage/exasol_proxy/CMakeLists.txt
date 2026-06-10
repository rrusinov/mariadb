OPTION(WITH_EXASOL_PROXY_STORAGE_ENGINE
  "Build the EXASOL proxy storage engine plugin (ha_exasol_proxy.so)" OFF)

IF(NOT WITH_EXASOL_PROXY_STORAGE_ENGINE)
  SET(PLUGIN_exasol_proxy NO CACHE STRING "How to build plugin exasol_proxy" FORCE)
  MESSAGE(STATUS "Skipping EXASOL proxy storage engine (enable with -DWITH_EXASOL_PROXY_STORAGE_ENGINE=ON)")
  RETURN()
ENDIF()

MESSAGE(STATUS "Building EXASOL proxy storage engine from the vendored ABI header")

SET(EXASOL_PROXY_SOURCES
  exasol_proxy_sql_generator.cc
  ha_exasol_proxy.cc)

MYSQL_ADD_PLUGIN(exasol_proxy ${EXASOL_PROXY_SOURCES} STORAGE_ENGINE MODULE_ONLY
  LINK_LIBRARIES ${CMAKE_DL_LIBS})
