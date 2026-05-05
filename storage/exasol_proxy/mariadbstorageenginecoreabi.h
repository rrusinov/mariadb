#ifndef MARIADBSTORAGEENGINECOREABI_H
#define MARIADBSTORAGEENGINECOREABI_H

/*
  Keep this ABI declaration in sync with db/Server/include/mariadbstorageenginecoreabi.h.
  The EXASOL proxy plugin builds against this standalone interface and resolves the
  core implementation from the preloaded ha_exasol.so at runtime via dlsym().
*/

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef __cplusplus
class THD;
struct TABLE;
#endif

enum
{
    EXASOL_MARIADB_CORE_ABI_VERSION_1 = 1
};

typedef struct ExasolMariaDBPushedQueryCursor ExasolMariaDBPushedQueryCursor;

typedef struct ExasolMariaDBCoreAbiV1
{
    unsigned int abiVersion;
    const char* (*getEngineName)();
    int (*initStorageEngine)(void* pluginData);
    int (*deinitStorageEngine)(void* pluginData);
    int (*stagePushedQueryResult)(THD* thd,
                                  const char* statementText,
                                  char* qualifiedTableNameBuffer,
                                  unsigned long qualifiedTableNameBufferSize,
                                  char* errorBuffer,
                                  unsigned long errorBufferSize);
    ExasolMariaDBPushedQueryCursor* (*openPushedQuery)(THD* thd,
                                                       TABLE* mariadbTable,
                                                       const char* statementText,
                                                       int clearTemporaryTablesOnClose,
                                                       char* errorBuffer,
                                                       unsigned long errorBufferSize);
    int (*fetchPushedQueryRow)(ExasolMariaDBPushedQueryCursor* cursor,
                               TABLE* mariadbTable,
                               unsigned char* record,
                               char* errorBuffer,
                               unsigned long errorBufferSize);
    int (*closePushedQuery)(ExasolMariaDBPushedQueryCursor* cursor,
                            char* errorBuffer,
                            unsigned long errorBufferSize);
} ExasolMariaDBCoreAbiV1;

const ExasolMariaDBCoreAbiV1* exasol_mariadb_get_core_abi_v1(void);

#ifdef __cplusplus
}
#endif

#endif
