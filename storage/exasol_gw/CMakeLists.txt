OPTION(WITH_EXASOL_GW_STORAGE_ENGINE
  "Build the Exasol Gateway proxy storage engine plugin (ha_exasol_gw.so)" OFF)

IF(NOT WITH_EXASOL_GW_STORAGE_ENGINE)
  SET(PLUGIN_exasol_gw NO CACHE STRING "How to build plugin exasol_gw" FORCE)
  MESSAGE(STATUS "Skipping Exasol Gateway storage engine (enable with -DWITH_EXASOL_GW_STORAGE_ENGINE=ON)")
  RETURN()
ENDIF()

MESSAGE(STATUS "Building Exasol Gateway proxy storage engine")

FIND_PACKAGE(SessionGatewaySdk CONFIG QUIET)
IF(NOT TARGET SessionGateway::Sdk)
  FIND_PATH(SESSIONGATEWAY_SDK_INCLUDE_DIR sessiongw/c_api.h)
  FIND_LIBRARY(SESSIONGATEWAY_SDK_LIBRARY NAMES SessionGatewaySdk)
  IF(NOT SESSIONGATEWAY_SDK_INCLUDE_DIR OR NOT SESSIONGATEWAY_SDK_LIBRARY)
    MESSAGE(FATAL_ERROR
      "SessionGatewaySdk is required. Set SessionGatewaySdk_DIR or "
      "SESSIONGATEWAY_SDK_INCLUDE_DIR and SESSIONGATEWAY_SDK_LIBRARY.")
  ENDIF()
  ADD_LIBRARY(SessionGateway::Sdk SHARED IMPORTED)
  SET_TARGET_PROPERTIES(SessionGateway::Sdk PROPERTIES
    IMPORTED_LOCATION "${SESSIONGATEWAY_SDK_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SESSIONGATEWAY_SDK_INCLUDE_DIR}")
ENDIF()

# Prefer the product-facing target while retaining compatibility with SDK
# packages that only export the original SessionGateway::Sdk target.
IF(NOT TARGET ExasolGateway::Sdk)
  ADD_LIBRARY(ExasolGateway::Sdk INTERFACE IMPORTED)
  SET_PROPERTY(TARGET ExasolGateway::Sdk PROPERTY
    INTERFACE_LINK_LIBRARIES SessionGateway::Sdk)
ENDIF()

SET(EXASOL_GW_SOURCES
  exasol_arrow_ipc.cc
  exasol_native_write_batch.cc
  exasol_gw_pushdown.cc
  exasol_gw_sql_generator.cc
  exasol_gw_session.cc
  exasol_sessiongw_client.cc
  ha_exasol_gw.cc)

MYSQL_ADD_PLUGIN(exasol_gw ${EXASOL_GW_SOURCES} STORAGE_ENGINE MODULE_ONLY
  LINK_LIBRARIES ExasolGateway::Sdk)
