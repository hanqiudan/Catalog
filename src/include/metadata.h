/*-------------------------------------------------------------------------
 *
 * metadata.h
 *    Metadata table accessors for the iceberg_catalog extension.
 *
 * This module provides the low-level SPI-based CRUD operations on the
 * Iceberg metadata tables (tables_internal, table_schemas, partition_specs).
 * Higher-level SQL functions (e.g. create_table) call these functions
 * rather than constructing SQL themselves.
 *
 *-------------------------------------------------------------------------
 */

#ifndef ICEBERG_CATALOG_METADATA_H
#define ICEBERG_CATALOG_METADATA_H

#include "postgres.h"

/*
 * MetaTableInfo -- fields that map to columns in iceberg_catalog.tables_internal.
 *
 * Boolean "has_" flags distinguish between "field is 0 / false"
 * and "field was not provided" (i.e. NULL in the database).
 */
typedef struct MetaTableInfo {
    Oid     relid;                         /* OID of the backing storage relation */
    char   *namespace_name;                /* Iceberg namespace (logical schema) */
    char   *table_name;                    /* Iceberg table name */
    char   *table_uuid;                    /* UUID assigned to this table */
    char   *metadata_location;             /* path/URL to v<N>.metadata.json */
    char   *previous_metadata_location;    /* prior metadata location (NULL for new tables) */
    char   *table_location;                /* Iceberg table root path */
    int     last_column_id;                /* highest column id assigned in the schema */
    int     current_schema_id;             /* ID of the current schema */
    bool    has_current_schema_id;         /* false if current_schema_id is not set */
    int64_t current_snapshot_id;           /* ID of the current snapshot */
    bool    has_current_snapshot_id;       /* false if no snapshot exists yet */
    int     default_spec_id;               /* ID of the default partition spec */
    bool    has_default_spec_id;           /* false if default_spec_id is not set */
} MetaTableInfo;

/*
 * MetaRegisterTableInput -- aggregate input for iceberg_meta_register_table().
 *
 * Combines the table head record (MetaTableInfo) with the schema and
 * partition-spec data that are expanded into the dependent tables.
 */
typedef struct MetaRegisterTableInput {
    MetaTableInfo table_info;              /* table head record fields */
    const char   *schema_json;             /* Iceberg struct schema as JSON */
    const char   *partition_fields_json;   /* partition spec fields as JSON */
    int           schema_id;               /* schema version identifier */
    int           spec_id;                 /* partition spec version identifier */
} MetaRegisterTableInput;

/*
 * MetaCommitTableInput -- input for iceberg_meta_commit_table().
 *
 * Delivered after SDK CommitTable succeeds.  update_table uses the
 * optimistic-lock pair (old_metadata_location, new_metadata_location)
 * and the snapshot fields are written into snapshots.
 */
typedef struct MetaCommitTableInput {
    const char *namespace_name;        /* table namespace */
    const char *table_name;            /* table name */
    const char *table_uuid;            /* UUID from the locked MetaTableInfo */
    const char *old_metadata_location; /* pre-commit metadata pointer (CAS key) */
    const char *new_metadata_location; /* post-commit metadata pointer */
    int64_t     new_snapshot_id;       /* snapshot-id from the SDK result */
    int         snapshot_schema_id;    /* schema_id for the snapshot row */
    bool        has_snapshot_schema_id;/* true --> write snapshot_schema_id */
    int64_t     snapshot_timestamp_ms; /* timestamp-ms from the SDK result */
    const char *manifest_list;         /* manifest-list path, may be NULL */
    int64_t     total_records;         /* summary.total-records */
    bool        has_total_records;     /* true --> write total_records */
} MetaCommitTableInput;

/*
 * MetaCommitSchemaChangeInput -- input for iceberg_meta_commit_schema_change().
 */
typedef struct MetaCommitSchemaChangeInput {
    const char *namespace_name;        /* table namespace */
    const char *table_name;            /* table name */
    const char *table_uuid;            /* UUID from the locked MetaTableInfo */
    const char *old_metadata_location; /* pre-commit metadata pointer (CAS key) */
    const char *new_metadata_location; /* post-commit metadata pointer */
    int         new_schema_id;         /* new current_schema_id to write */
    const char *schema_json;           /* new schema JSON for table_schemas */
    int         new_last_column_id;    /* new last_column_id to write */
} MetaCommitSchemaChangeInput;

/*
 * Check whether an Iceberg namespace exists in the local catalog.
 * Returns true if a row with matching catalog_name and namespace is found.
 */
bool iceberg_meta_namespace_exists(const char *namespace_name);

/*
 * Check whether a table already exists within the given namespace.
 * Both namespace_name and table_name are required (non-empty).
 */
bool iceberg_meta_table_exists(const char *namespace_name, const char *table_name);

/*
 * Register a new Iceberg table in the local metadata tables.
 *
 * Within a single SPI transaction this function:
 *  1. Locks the namespace row for share (prevents concurrent creation races).
 *  2. Inserts the table head record into tables_internal.
 *  3. Expands the schema JSON into table_schemas.
 *  4. Expands the partition spec JSON into partition_specs.
 *
 * The caller is responsible for ensuring the namespace exists and the
 * table name is not already taken.
 */
void iceberg_meta_register_table(const char *namespace_name,
                                 const char *table_name,
                                 const MetaRegisterTableInput *input);

/*
 * Free a MetaTableInfo structure and all of its palloc'd string members.
 * Safe to call with NULL (no-op).
 */
void iceberg_meta_free_table_info(MetaTableInfo *info);

/*
 * List tables within the given namespace, with cursor-based pagination.
 *
 * Returns a palloc'd JSON string (ListTablesResponse) of the shape:
 *   {"identifiers":[{"namespace":["ns"],"name":"t1"},...],
 *    "next-page-token":"..." or null}
 *
 * The caller must pfree() the result.
 */
char *iceberg_meta_list_tables(const char *namespace_name,
                               int page_size,
                               const char *page_token);

/*
 * Look up a table in the local catalog by namespace and table name.
 * Returns a palloc'd MetaTableInfo if found, or NULL if no row exists.
 * The caller must free the result with iceberg_meta_free_table_info().
 */
MetaTableInfo *iceberg_meta_get_table(const char *namespace_name,
                                      const char *table_name);

/*
 * Lock a table row for write-path operations (SELECT ... FOR UPDATE).
 * Returns a palloc'd MetaTableInfo; caller must free via iceberg_meta_free_table_info.
 * Returns NULL if the table does not exist.
 */
MetaTableInfo *iceberg_meta_get_table_for_update(const char *namespace_name,
                                                  const char *table_name);

/*
 * Lock an Iceberg table row for update and return its metadata.
 *
 * This is a service function that manages its own SPI connect/finish.
 * Returns a palloc'd MetaTableInfo.  Raises ERRCODE_UNDEFINED_OBJECT
 * (mapped to P0004 Not Found) if no row exists.
 */
MetaTableInfo *iceberg_meta_lock_table(const char *namespace_name,
                                       const char *table_name);

/*
 * Rename a table in the local metadata tables (service wrapper).
 *
 * Connects SPI, validates preconditions (source exists, destination does not),
 * performs the rename via UPDATE, and finishes SPI.  Errors are translated
 * via the internal throw_translated_spi_error pattern.
 */
void iceberg_meta_rename_table_record(const char *src_ns, const char *src_table,
                                      const char *dst_ns, const char *dst_table);

/*
 * Delete the table record from iceberg_catalog.tables_internal.
 *
 * This is a service function that manages its own SPI connect/finish.
 * Raises ERRCODE_UNDEFINED_OBJECT if no matching row is found.
 * ON DELETE CASCADE on child tables (table_schemas, partition_specs)
 * cleans up dependent rows automatically.
 */
void iceberg_meta_drop_table_record(const char *namespace_name,
                                    const char *table_name);

/*
 * Update table metadata pointers and optional summary fields with optimistic locking.
 * Uses CAS: WHERE metadata_location = old + table_uuid check.
 * This is an internal function; SQL functions should use the scene-level
 * commit wrappers instead.
 */
void iceberg_meta_update_table(const char *namespace_name,
                               const char *table_name,
                               const char *table_uuid,
                               const char *old_metadata_location,
                               const char *new_metadata_location,
                               int64_t new_snapshot_id,
                               bool has_new_snapshot_id,
                               int new_schema_id,
                               bool has_new_schema_id,
                               int new_last_column_id,
                               bool has_new_last_column_id,
                               int new_default_spec_id,
                               bool has_new_default_spec_id);

/*
 * Insert a snapshot summary row into iceberg_catalog.snapshots.
 * Internal function; does not manage SPI.
 */
void iceberg_meta_insert_snapshot(const char *table_uuid,
                                   int64_t snapshot_id,
                                   int schema_id,
                                   bool has_schema_id,
                                   int64_t timestamp_ms,
                                   const char *manifest_list,
                                   int64_t total_records,
                                   bool has_total_records);

/*
 * Scene-level commit: update table pointer + insert snapshot row.
 * This is the primary entry-point for commit_table.
 */
void iceberg_meta_commit_table(const MetaCommitTableInput *input);

/*
 * Scene-level schema change commit: update table pointer + insert schema row.
 * This is the primary entry-point for add_column.
 */
void iceberg_meta_commit_schema_change(const MetaCommitSchemaChangeInput *input);

#endif /* ICEBERG_CATALOG_METADATA_H */
