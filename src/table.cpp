/*-------------------------------------------------------------------------
 *
 * table.cpp
 *    Iceberg table SQL function implementations.
 *
 * Stub implementation: all openGauss catalog metadata-table operations
 * and Iceberg SDK calls are marked as TODO, pending the underlying
 * modules to be wired up. Currently returns a minimal response.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"

#include <string.h>

#include "iceberg_catalog.h"
#include "table.h"


/* ---- create_table ---- */

PG_FUNCTION_INFO_V1(iceberg_create_table);

Datum
iceberg_create_table(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace       TEXT     (required)
     *   2. p_table_name      TEXT     (required)
     *   3. p_schema          JSONB    (required)
     *   4. p_location        TEXT     (optional, default NULL)
     *   5. p_partition_spec  JSONB    (optional, default NULL)
     *   6. p_write_order     JSONB    (optional, default NULL)
     *   7. p_stage_create    BOOLEAN  (optional, default FALSE)
     *   8. p_properties      JSONB    (optional, default NULL)
     *
     * Returns: JSONB (LoadTableResult)
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 3)
        elog(ERROR, "iceberg_create_table: expected at least 3 arguments, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_table_name (required) */
    char *p_table_name = NULL;
    if (!PG_ARGISNULL(1))
        p_table_name = text_to_cstring(PG_GETARG_TEXT_P(1));

    /* p_schema (required) */
    Jsonb *p_schema = NULL;
    if (!PG_ARGISNULL(2))
        p_schema = DatumGetJsonb(PG_GETARG_DATUM(2));

    /* p_location (optional, default NULL) */
    char *p_location = NULL;
    if (PG_NARGS() > 3 && !PG_ARGISNULL(3))
        p_location = text_to_cstring(PG_GETARG_TEXT_P(3));

    /* p_partition_spec (optional, default NULL) */
    Jsonb *p_partition_spec = NULL;
    if (PG_NARGS() > 4 && !PG_ARGISNULL(4))
        p_partition_spec = DatumGetJsonb(PG_GETARG_DATUM(4));

    /* p_write_order (optional, default NULL) */
    Jsonb *p_write_order = NULL;
    if (PG_NARGS() > 5 && !PG_ARGISNULL(5))
        p_write_order = DatumGetJsonb(PG_GETARG_DATUM(5));

    /* p_stage_create (optional, default FALSE) */
    bool p_stage_create = false;
    if (PG_NARGS() > 6 && !PG_ARGISNULL(6))
        p_stage_create = PG_GETARG_BOOL(6);

    /* p_properties (optional, default NULL) */
    Jsonb *p_properties = NULL;
    if (PG_NARGS() > 7 && !PG_ARGISNULL(7))
        p_properties = DatumGetJsonb(PG_GETARG_DATUM(7));

    /* 2. Validate required parameters */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_namespace is required and must not be empty")));

    if (p_table_name == NULL || strlen(p_table_name) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_table_name is required and must not be empty")));

    if (p_schema == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_schema is required and must not be NULL")));

    /* 3. TODO: Validate p_schema type == "struct", ValidateType for each field */

    /* TODO: Validate p_schema type is "struct" */
    /* TODO: For each field in p_schema.fields[], call catalog->ValidateType(field.type) */

    /* 4. TODO: Check namespace exists */

    /* TODO:
     * if (!iceberg_meta_namespace_exists(p_namespace))
     *     iceberg_error(ERRCODE_ICEBERG_NOT_FOUND,
     *                   "NoSuchNamespaceException",
     *                   "Namespace does not exist");
     */

    /* 5. TODO: Check table does not already exist */

    /* TODO:
     * if (iceberg_meta_table_exists(p_namespace, p_table_name))
     *     iceberg_error(ERRCODE_ICEBERG_CONFLICT,
     *                   "AlreadyExistsException",
     *                   "Table already exists");
     */

    /* 6. TODO: SDK CreateTable */

    /* TODO:
     * IcebergCatalog *catalog = get_iceberg_catalog();
     * LoadTableResult result = catalog->CreateTable(
     *     p_namespace, p_table_name, p_schema,
     *     p_location, p_partition_spec, p_write_order,
     *     p_stage_create, p_properties);
     * if (result.error)
     *     iceberg_error(ERRCODE_ICEBERG_INTERNAL_ERROR,
     *                   "RuntimeException",
     *                   "Failed to create table via SDK");
     */

    /* 7. TODO: DDL CreateStorage */

    /* TODO:
     * iceberg_ddl_CreateStorage(p_namespace, p_table_name, result);
     */

    /* 8. TODO: META InsertTable */

    /* TODO:
     * iceberg_meta_register_table(p_namespace, p_table_name, result);
     */

    /* 9. TODO: Construct and return JSONB response */

    /* TODO:
     * StringInfo buf = makeStringInfo();
     * appendStringInfo(buf,
     *     "{\"metadata-location\":\"%s\",\"metadata\":{...},\"config\":{...}}",
     *     result.metadata_location);
     * Jsonb *ret = ...;
     * PG_RETURN_JSONB_P(ret);
     */

    /* 10. Return minimal JSONB response (TODO: replace with real data from SDK/META) */

    /* TODO: Build response from IcebergTable returned by catalog->CreateTable()
     * once SDK & META modules are available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"metadata-location\": \"TODO\", \"metadata\": {}, \"config\": {}}")));
}


/* ---- commit_table ---- */

PG_FUNCTION_INFO_V1(iceberg_commit_table);

Datum
iceberg_commit_table(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace    TEXT   (required)
     *   2. p_table        TEXT   (required)
     *   3. p_requirements JSONB  (required)
     *   4. p_updates      JSONB  (required)
     *
     * Returns: JSONB
     *   {"metadata-location": "<path>", "metadata": {...}}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 4)
        elog(ERROR, "iceberg_commit_table: expected at least 4 arguments, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_table (required) */
    char *p_table = NULL;
    if (!PG_ARGISNULL(1))
        p_table = text_to_cstring(PG_GETARG_TEXT_P(1));

    /* p_requirements (required) */
    Jsonb *p_requirements = NULL;
    if (!PG_ARGISNULL(2))
        p_requirements = DatumGetJsonb(PG_GETARG_DATUM(2));

    /* p_updates (required) */
    Jsonb *p_updates = NULL;
    if (!PG_ARGISNULL(3))
        p_updates = DatumGetJsonb(PG_GETARG_DATUM(3));

    /* 2. Validate required parameters */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_namespace is required and must not be empty")));

    if (p_table == NULL || strlen(p_table) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_table is required and must not be empty")));

    if (p_requirements == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_requirements is required and must not be NULL")));

    if (p_updates == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_updates is required and must not be NULL")));

    /* 3. TODO: Validate p_updates elements have action = "add-snapshot" */

    /* TODO: Validate each element in p_updates has "action" = "add-snapshot" */

    /* 4. TODO: META GetTableForUpdate */

    /* TODO:
     * info = META.GetTableForUpdate(p_namespace, p_table);
     * if (info == NULL)
     *     ereport(ERROR, (errcode(ERRCODE_ICEBERG_NOT_FOUND),
     *                     errmsg("table not found")));
     */

    /* 5. TODO: SDK LoadTable */

    /* TODO:
     * error_msg = NULL;
     * table = catalog->LoadTable(p_namespace, p_table, info->metadata_location, &error_msg);
     * if (error_msg != NULL)
     *     ereport(ERROR, (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
     *                     errmsg("{\"type\":\"ServiceUnavailable\",\"message\":\"%s\",\"stack\":[]}", error_msg)));
     */

    /* 6. TODO: SDK CommitTable (apply requirements + updates + write S3) */

    /* TODO:
     * newMdlLocation = table->CommitTable(jsonb_to_cstring(p_requirements),
     *                                      jsonb_to_cstring(p_updates), &error_msg);
     * if (error_msg != NULL)
     *     ereport(ERROR, (errcode(ERRCODE_ICEBERG_CONFLICT),
     *                     errmsg("{\"type\":\"CommitFailedException\",\"message\":\"%s\",\"stack\":[]}", error_msg)));
     */

    /* 7. TODO: META UpdateTable (optimistic lock) */

    /* TODO:
     * newSnapshotId = get_snapshot_id_from(p_updates);
     * META.UpdateTable(p_namespace, p_table,
     *                   info->metadata_location,
     *                   newMdlLocation,
     *                   newSnapshotId,
     *                   table->GetCurrentSchemaId(),
     *                   table->GetLastColumnId());
     */

    /* 8. TODO: Construct and return JSONB response */

    /* TODO:
     * metadata_json = table->GetMetadataJson();
     * delete table;
     * return {
     *     "metadata-location": newMdlLocation,
     *     "metadata":          json_parse(metadata_json)
     * };
     */

    /* 9. Return minimal JSONB response (TODO: replace with real data from SDK/META) */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"metadata-location\": \"TODO\", \"metadata\": {}}")));
}


/* ---- add_column ---- */

PG_FUNCTION_INFO_V1(iceberg_add_column);

Datum
iceberg_add_column(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace   TEXT    (required)
     *   2. p_table       TEXT    (required)
     *   3. p_column_name TEXT    (required)
     *   4. p_column_type TEXT    (required)
     *   5. p_column_doc  TEXT    (optional, default NULL)
     *
     * Returns: JSONB
     *   {"metadata-location": "<path>", "metadata": {...}}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 4)
        elog(ERROR, "iceberg_add_column: expected at least 4 arguments, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_table (required) */
    char *p_table = NULL;
    if (!PG_ARGISNULL(1))
        p_table = text_to_cstring(PG_GETARG_TEXT_P(1));

    /* p_column_name (required) */
    char *p_column_name = NULL;
    if (!PG_ARGISNULL(2))
        p_column_name = text_to_cstring(PG_GETARG_TEXT_P(2));

    /* p_column_type (required) */
    char *p_column_type = NULL;
    if (!PG_ARGISNULL(3))
        p_column_type = text_to_cstring(PG_GETARG_TEXT_P(3));

    /* p_column_doc (optional, default NULL) */
    char *p_column_doc = NULL;
    if (PG_NARGS() > 4 && !PG_ARGISNULL(4))
        p_column_doc = text_to_cstring(PG_GETARG_TEXT_P(4));

    /* 2. Validate required parameters */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_namespace is required and must not be empty")));

    if (p_table == NULL || strlen(p_table) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_table is required and must not be empty")));

    if (p_column_name == NULL || strlen(p_column_name) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_column_name is required and must not be empty")));

    if (p_column_type == NULL || strlen(p_column_type) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_column_type is required and must not be empty")));

    /* 3. TODO: Validate p_column_type via SDK */

    /* TODO:
     * if (!catalog->ValidateType(p_column_type, &err))
     *     ereport(ERROR, (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
     *                     errmsg("invalid column type: %s", err)));
     */

    /* 4. TODO: META GetTableForUpdate */

    /* TODO:
     * info = META.GetTableForUpdate(p_namespace, p_table);
     * if (info == NULL)
     *     ereport(ERROR, (errcode(ERRCODE_ICEBERG_NOT_FOUND),
     *                     errmsg("table not found")));
     */

    /* 5. TODO: SDK LoadTable */

    /* TODO:
     * error_msg = NULL;
     * table = catalog->LoadTable(p_namespace, p_table, info->metadata_location, &error_msg);
     * if (error_msg != NULL)
     *     ereport(ERROR, (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
     *                     errmsg("{\"type\":\"ServiceUnavailable\",\"message\":\"%s\",\"stack\":[]}", error_msg)));
     */

    /* 6. TODO: Check column name conflict against current schema */

    /* TODO:
     * currentSchema = table->GetCurrentSchema();
     * if currentSchema has field named p_column_name → ereport(P0001, "column already exists")
     */

    /* 7. TODO: SDK AddColumn → new schema + new field ID */

    /* TODO:
     * newFieldId = 0;
     * newSchema = table->AddColumn(p_column_name, p_column_type, p_column_doc, &newFieldId);
     * newSchemaId = table->GetCurrentSchemaId() + 1;
     */

    /* 8. TODO: Auto-construct requirements and updates */

    /* TODO:
     * requirements = [
     *     {"type":"assert-table-uuid",            "uuid": info.table_uuid},
     *     {"type":"assert-ref-snapshot-id",       "ref":"main", "snapshot-id": info.current_snapshot_id},
     *     {"type":"assert-current-schema-id",     "current-schema-id": info.current_schema_id},
     *     {"type":"assert-last-assigned-field-id","last-assigned-field-id": info.last_column_id}
     * ]
     * updates = [
     *     {"action":"add-schema",         "schema": json_parse(newSchema->GetSchemaJson()),
     *                                      "last-column-id": newFieldId},
     *     {"action":"set-current-schema", "schema-id": newSchemaId}
     * ]
     */

    /* 9. TODO: SDK CommitTable (apply changes + write S3) */

    /* TODO:
     * newMdlLocation = table->CommitTable(jsonb_to_cstring(requirements),
     *                                      jsonb_to_cstring(updates), &error_msg);
     * if (error_msg != NULL)
     *     ereport(ERROR, (errcode(ERRCODE_ICEBERG_CONFLICT),
     *                     errmsg("{\"type\":\"CommitFailedException\",\"message\":\"%s\",\"stack\":[]}", error_msg)));
     */

    /* 10. TODO: META UpdateTable (optimistic lock) */

    /* TODO:
     * META.UpdateTable(p_namespace, p_table,
     *                   info->metadata_location,
     *                   newMdlLocation,
     *                   info->current_snapshot_id,
     *                   newSchemaId,
     *                   newFieldId);
     */

    /* 11. TODO: Construct and return JSONB response */

    /* TODO:
     * metadata_json = table->GetMetadataJson();
     * delete table;
     * return {
     *     "metadata-location": newMdlLocation,
     *     "metadata":          json_parse(metadata_json)
     * };
     */

    /* 12. Return minimal JSONB response (TODO: replace with real data from SDK/META) */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"metadata-location\": \"TODO\", \"metadata\": {}}")));
}
