/*-------------------------------------------------------------------------
 *
 * metadata.cpp
 *    Metadata table accessors for the iceberg_catalog extension.
 *
 * All catalog metadata mutations go through SPI (Server Programming Interface).
 * Each public function manages its own SPI connect/finish pair unless it is
 * called from another function that already holds the connection
 * (see iceberg_meta_register_table which wraps multiple operations in one
 * SPI transaction).
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"

#include <string.h>

#include "iceberg_catalog.h"
#include "metadata.h"

/* Convenience macro wrapping SPI_execute_with_args with NULL resource owner */
#define ICEBERG_SPI_EXECUTE_WITH_ARGS(src, nargs, argtypes, values, nulls, read_only, tcount) \
    SPI_execute_with_args(src, nargs, argtypes, values, nulls, read_only, tcount, NULL)

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* Returns true if value is NULL or an empty string. */
static bool
is_empty_string(const char *value)
{
    return value == NULL || value[0] == '\0';
}

/* Raises an ERROR if value is NULL or empty.  name is used in the error message. */
static void
validate_name(const char *value, const char *name)
{
    if (is_empty_string(value))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s is required and must not be empty", name)));
}

/* ------------------------------------------------------------------ */
/*  SPI lifecycle helpers                                              */
/* ------------------------------------------------------------------ */

static void
connect_spi(void)
{
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to connect to SPI")));
}

static void
finish_spi(void)
{
    if (SPI_finish() != SPI_OK_FINISH)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to finish SPI")));
}

static bool
is_metadata_sqlstate(int sqlerrcode)
{
    return sqlerrcode == ERRCODE_INVALID_PARAMETER_VALUE ||
           sqlerrcode == ERRCODE_UNDEFINED_OBJECT ||
           sqlerrcode == ERRCODE_DUPLICATE_OBJECT ||
           sqlerrcode == ERRCODE_T_R_SERIALIZATION_FAILURE ||
           sqlerrcode == ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE ||
           sqlerrcode == ERRCODE_FEATURE_NOT_SUPPORTED ||
           sqlerrcode == ERRCODE_DATA_CORRUPTED ||
           sqlerrcode == ERRCODE_INTERNAL_ERROR;
}

static void
finish_spi_quietly(bool *spi_connected)
{
    if (spi_connected != NULL && *spi_connected) {
        /* ERROR cleanup path: ignore SPI_finish failures and preserve the original error. */
        (void) SPI_finish();
        *spi_connected = false;
    }
}

/*
 * Re-throw metadata SQLSTATEs unchanged.  Raw database/SPI errors are
 * normalized to the metadata module's standard SQLSTATE contract after SPI has
 * been closed.
 */
static void
throw_translated_spi_error(ErrorData *edata, const char *context)
{
    int sqlerrcode = edata->sqlerrcode;
    char *message;

    if (is_metadata_sqlstate(sqlerrcode)) {
        FreeErrorData(edata);
        PG_RE_THROW();
    }

    message = edata->message == NULL ? pstrdup("metadata SPI operation failed")
                                     : pstrdup(edata->message);
    FreeErrorData(edata);
    FlushErrorState();

    if (sqlerrcode == ERRCODE_UNIQUE_VIOLATION)
        ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg("%s: %s", context, message)));

    if (sqlerrcode == ERRCODE_INVALID_TEXT_REPRESENTATION ||
        sqlerrcode == ERRCODE_INVALID_PARAMETER_VALUE)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: %s", context, message)));

    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("%s: %s", context, message)));
}

/*
 * Execute a single-parameter SELECT that returns at most one row,
 * and return whether any row was returned.
 */
static bool
execute_exists_query(const char *sql, Datum *values, Oid *argtypes)
{
    int rc;

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(sql, 1, argtypes, values, NULL, true, 1);
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("metadata exists query failed")));

    return SPI_processed > 0;
}

/* ------------------------------------------------------------------ */
/*  Namespace operations                                               */
/* ------------------------------------------------------------------ */

/*
 * Check whether a namespace exists.
 * Queries iceberg_catalog.namespaces filtered by current_database().
 */
bool
iceberg_meta_namespace_exists(const char *namespace_name)
{
    Datum values[1];
    Oid argtypes[1] = {TEXTOID};
    bool exists = false;
    bool spi_connected = false;

    validate_name(namespace_name, "namespace_name");

    values[0] = CStringGetTextDatum(namespace_name);

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;
        exists = execute_exists_query(
            "SELECT 1 "
            "FROM iceberg_catalog.namespaces "
            "WHERE catalog_name = current_database()::text "
            "  AND namespace = $1",
            values,
            argtypes);
        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        /* Save the original error before SPI cleanup can overwrite it. */
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata namespace exists query");
    }
    PG_END_TRY();

    return exists;
}

/* ------------------------------------------------------------------ */
/*  Table existence checks                                             */
/* ------------------------------------------------------------------ */

/*
 * Check whether a table already exists in the given namespace.
 * Consults iceberg_catalog.tables_internal.
 */
bool
iceberg_meta_table_exists(const char *namespace_name, const char *table_name)
{
    Datum values[2];
    Oid argtypes[2] = {TEXTOID, TEXTOID};
    bool exists = false;
    bool spi_connected = false;
    int rc;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");

    values[0] = CStringGetTextDatum(namespace_name);
    values[1] = CStringGetTextDatum(table_name);

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;
        rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
            "SELECT 1 "
            "FROM iceberg_catalog.tables_internal "
            "WHERE namespace = $1 "
            "  AND table_name = $2",
            2,
            argtypes,
            values,
            NULL,
            true,
            1);
        if (rc != SPI_OK_SELECT)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("metadata table exists query failed")));

        exists = SPI_processed > 0;
        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        /* Save the original error before SPI cleanup can overwrite it. */
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata table exists query");
    }
    PG_END_TRY();

    return exists;
}

/* ------------------------------------------------------------------ */
/*  Table registration  (transactional, multi-statement)               */
/* ------------------------------------------------------------------ */

/*
 * Lock the namespace row with FOR SHARE to prevent concurrent
 * create-table-within-the-same-namespace races.
 * Raises ERROR if the namespace disappears between the check and the lock.
 */
static void
lock_namespace_for_share(const char *namespace_name)
{
    Datum values[1];
    Oid argtypes[1] = {TEXTOID};
    int rc;

    values[0] = CStringGetTextDatum(namespace_name);

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "SELECT 1 "
        "FROM iceberg_catalog.namespaces "
        "WHERE catalog_name = current_database()::text "
        "  AND namespace = $1 "
        "FOR SHARE",
        1,
        argtypes,
        values,
        NULL,
        false,
        1);
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to lock namespace metadata")));

    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("namespace not found")));
}

/*
 * Insert the table head record into iceberg_catalog.tables_internal.
 *
 * The previous_metadata_location field is intentionally NULL for new tables
 * (the caller must validate this before calling).
 */
static void
insert_table_record(const char *namespace_name,
                    const char *table_name,
                    const MetaTableInfo *info)
{
    Datum values[11];
    Oid argtypes[11] = {
        OIDOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID,
        INT4OID, INT4OID, INT8OID, INT4OID};
    char nulls[11] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    int rc;

    values[0] = ObjectIdGetDatum(info->relid);
    values[1] = CStringGetTextDatum(namespace_name);
    values[2] = CStringGetTextDatum(table_name);
    values[3] = CStringGetTextDatum(info->table_uuid);
    values[4] = CStringGetTextDatum(info->metadata_location);
    values[5] = (Datum) 0;
    nulls[5] = 'n';                                    /* previous_metadata_location (NULL for new table) */
    values[6] = CStringGetTextDatum(info->table_location);
    values[7] = Int32GetDatum(info->last_column_id);
    values[8] = Int32GetDatum(info->current_schema_id);
    if (!info->has_current_snapshot_id)
        nulls[9] = 'n';                                /* current_snapshot_id (NULL when no snapshot yet) */
    else
        values[9] = Int64GetDatum(info->current_snapshot_id);
    values[10] = Int32GetDatum(info->default_spec_id);

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "INSERT INTO iceberg_catalog.tables_internal("
        "    relid, namespace, table_name, table_uuid,"
        "    metadata_location, previous_metadata_location, table_location,"
        "    last_column_id, current_schema_id, current_snapshot_id, default_spec_id"
        ") VALUES ("
        "    $1, $2, $3, $4::uuid,"
        "    $5, $6, $7,"
        "    $8, $9, $10, $11"
        ")",
        11,
        argtypes,
        values,
        nulls,
        false,
        0);
    if (rc != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to insert table metadata")));

    if (SPI_processed != 1)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("unexpected table metadata insert count")));
}

/*
 * Expand the schema JSON into iceberg_catalog.table_schemas.
 *
 * Each field in the Iceberg struct schema produces one row.
 * The schema JSON must have the shape:
 *   {"type":"struct", "fields":[{"id":N, "name":"...", "required":bool, "type":"..."}, ...]}
 */
static void
insert_schema_fields(const char *table_uuid, int schema_id, const char *schema_json)
{
    Datum insert_values[3];
    Oid argtypes[3] = {TEXTOID, INT4OID, TEXTOID};
    int rc;

    insert_values[0] = CStringGetTextDatum(table_uuid);
    insert_values[1] = Int32GetDatum(schema_id);
    insert_values[2] = CStringGetTextDatum(schema_json);

    /*
     * Validate before INSERT so a partially invalid schema cannot silently
     * drop fields and leave table_schemas inconsistent with metadata.json.
     */
    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "WITH schema_data AS ("
        "    SELECT CASE "
        "        WHEN jsonb_typeof($1::jsonb) = 'object' "
        "         AND $1::jsonb->>'type' = 'struct' "
        "         AND jsonb_typeof($1::jsonb->'fields') = 'array' "
        "        THEN $1::jsonb->'fields' "
        "        ELSE NULL::jsonb "
        "    END AS fields"
        ") "
        "SELECT "
        "    fields IS NOT NULL,"
        "    CASE WHEN fields IS NULL THEN 0 ELSE jsonb_array_length(fields) END::bigint,"
        "    CASE WHEN fields IS NULL THEN 0 ELSE ("
        "        SELECT count(*) "
        "        FROM jsonb_array_elements(fields) AS elems(field_value) "
        "        WHERE jsonb_typeof(field_value) = 'object' "
        "          AND field_value ? 'id' "
        "          AND field_value ? 'name' "
        "          AND field_value ? 'required' "
        "          AND field_value ? 'type' "
        "    ) END::bigint "
        "FROM schema_data",
        1,
        &argtypes[2],
        &insert_values[2],
        NULL,
        true,
        1);
    if (rc != SPI_OK_SELECT || SPI_processed != 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("failed to validate schema metadata")));
    else {
        bool isnull;
        bool valid_shape = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0],
                                                      SPI_tuptable->tupdesc,
                                                      1,
                                                      &isnull));
        int64 total_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                        SPI_tuptable->tupdesc,
                                                        2,
                                                        &isnull));
        int64 valid_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                        SPI_tuptable->tupdesc,
                                                        3,
                                                        &isnull));

        if (!valid_shape)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("schema must be a JSON struct with a fields array")));
        if (total_count != valid_count)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("schema fields must include id, name, required, and type")));
    }

    /* Use JSON array indexes as field_position; SQL row order is not a contract. */
    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "WITH schema_data AS ("
        "    SELECT $3::jsonb->'fields' AS fields"
        "), field_items AS ("
        "    SELECT "
        "        field_position::int AS field_position,"
        "        fields->(field_position::int) AS field_value "
        "    FROM schema_data, "
        "         generate_series(0, jsonb_array_length(fields) - 1) AS indexes(field_position)"
        ") "
        "INSERT INTO iceberg_catalog.table_schemas("
        "    table_uuid, schema_id, field_position,"
        "    field_id, field_name, field_required, field_type, field_doc"
        ") "
        "SELECT "
        "    $1::uuid,"
        "    $2,"
        "    field_position,"
        "    (field_value->>'id')::int,"
        "    field_value->>'name',"
        "    (field_value->>'required')::boolean,"
        "    CASE "
        "        WHEN jsonb_typeof(field_value->'type') = 'string' "
        "        THEN field_value->>'type' "
        "        ELSE (field_value->'type')::text "
        "    END,"
        "    field_value->>'doc' "
        "FROM field_items",
        3,
        argtypes,
        insert_values,
        NULL,
        false,
        0);
    if (rc != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("failed to insert schema metadata")));
}

/*
 * Expand the partition spec JSON into iceberg_catalog.partition_specs.
 *
 * Accepts three input shapes:
 *   - object with a "fields" array  -> object wrapping fields
 *   - object without "fields"       -> treated as empty spec (sentinel row)
 *   - bare array                    -> used directly as fields list
 *
 * An unpartitioned spec produces a sentinel row with field_position = -1.
 *
 * NOTE: openGauss requires the `?` operator (not `-> 'key' IS NULL`) to
 * check for key existence, because `IS NULL` on a jsonb expression does
 * not reliably return boolean in openGauss 6.x.
 */
static void
insert_partition_spec(const char *table_uuid, int spec_id, const char *fields_json)
{
    Datum values[3];
    Oid argtypes[3] = {TEXTOID, INT4OID, TEXTOID};
    const char *json = is_empty_string(fields_json) ? "[]" : fields_json;
    int rc;

    values[0] = CStringGetTextDatum(table_uuid);
    values[1] = Int32GetDatum(spec_id);
    values[2] = CStringGetTextDatum(json);

    /*
     * Validate before INSERT so mixed valid/invalid partition fields fail as
     * one unit instead of storing a truncated partition spec.
     */
    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "WITH spec AS ("
        "    SELECT CASE "
        "        WHEN jsonb_typeof($1::jsonb) = 'object' "
        "         AND jsonb_typeof($1::jsonb->'fields') = 'array' "
        "        THEN $1::jsonb->'fields' "
        "        WHEN jsonb_typeof($1::jsonb) = 'object' "
        "         AND NOT ($1::jsonb ? 'fields') "
        "        THEN '[]'::jsonb "
        "        WHEN jsonb_typeof($1::jsonb) = 'array' "
        "        THEN $1::jsonb "
        "        ELSE NULL::jsonb "
        "    END AS fields"
        ") "
        "SELECT "
        "    fields IS NOT NULL,"
        "    CASE WHEN fields IS NULL THEN 0 ELSE jsonb_array_length(fields) END::bigint,"
        "    CASE WHEN fields IS NULL THEN 0 ELSE ("
        "        SELECT count(*) "
        "        FROM jsonb_array_elements(fields) AS elems(field_value) "
        "        WHERE jsonb_typeof(field_value) = 'object' "
        "          AND field_value ? 'field-id' "
        "          AND field_value ? 'source-id' "
        "          AND field_value ? 'name' "
        "          AND field_value ? 'transform' "
        "    ) END::bigint "
        "FROM spec",
        1,
        &argtypes[2],
        &values[2],
        NULL,
        true,
        1);
    if (rc != SPI_OK_SELECT || SPI_processed != 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("failed to validate partition spec metadata")));
    else {
        bool isnull;
        bool valid_shape = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0],
                                                      SPI_tuptable->tupdesc,
                                                      1,
                                                      &isnull));
        int64 total_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                        SPI_tuptable->tupdesc,
                                                        2,
                                                        &isnull));
        int64 valid_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                        SPI_tuptable->tupdesc,
                                                        3,
                                                        &isnull));

        if (!valid_shape)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("partition spec must be a JSON array or object")));
        if (total_count != valid_count)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("partition fields must include field-id, source-id, name, and transform")));
    }

    /* Use JSON array indexes as field_position; SQL row order is not a contract. */
    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "WITH spec AS ("
        "    SELECT CASE "
        "        WHEN jsonb_typeof($3::jsonb) = 'object' "
        "         AND jsonb_typeof($3::jsonb->'fields') = 'array' "
        "        THEN $3::jsonb->'fields' "
        "        WHEN jsonb_typeof($3::jsonb) = 'object' "
        "         AND NOT ($3::jsonb ? 'fields') "
        "        THEN '[]'::jsonb "
        "        WHEN jsonb_typeof($3::jsonb) = 'array' "
        "        THEN $3::jsonb "
        "        ELSE NULL::jsonb "
        "    END AS fields"
        "), field_items AS ("
        "    SELECT "
        "        field_position::int AS field_position,"
        "        fields->(field_position::int) AS field_value "
        "    FROM spec, "
        "         generate_series(0, jsonb_array_length(fields) - 1) AS indexes(field_position)"
        ") "
        "INSERT INTO iceberg_catalog.partition_specs("
        "    table_uuid, spec_id, field_position,"
        "    field_id, source_id, field_name, transform"
        ") "
        "SELECT $1::uuid, $2, -1, NULL, NULL, NULL, NULL "
        "FROM spec "
        "WHERE fields IS NOT NULL "
        "  AND jsonb_array_length(fields) = 0 "
        "UNION ALL "
        "SELECT "
        "    $1::uuid,"
        "    $2,"
        "    field_position::int,"
        "    (field_value->>'field-id')::int,"
        "    (field_value->>'source-id')::int,"
        "    field_value->>'name',"
        "    CASE "
        "        WHEN jsonb_typeof(field_value->'transform') = 'string' "
        "        THEN field_value->>'transform' "
        "        ELSE (field_value->'transform')::text "
        "    END "
        "FROM field_items",
        3,
        argtypes,
        values,
        NULL,
        false,
        0);
    if (rc != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("failed to insert partition spec metadata")));

    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("partition spec must be a JSON array")));
}

/* ------------------------------------------------------------------ */
/*  Table registration (public)                                        */
/* ------------------------------------------------------------------ */

/*
 * Register a new Iceberg table in the local metadata tables.
 *
 * This is the main entry-point for table registration.  It runs entirely
 * within one SPI transaction:
 *
 *   1. Lock the namespace row (FOR SHARE) to serialize concurrent creation.
 *   2. Insert the table head record into tables_internal.
 *   3. Expand and insert schema fields into table_schemas.
 *   4. Expand and insert partition spec fields into partition_specs.
 *
 * All input pointers must remain valid for the duration of the call.
 */
void
iceberg_meta_register_table(const char *namespace_name,
                            const char *table_name,
                            const MetaRegisterTableInput *input)
{
    const MetaTableInfo *info;
    bool spi_connected = false;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");
    if (input == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("input is required")));

    info = &input->table_info;
    if (!OidIsValid(info->relid))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table relid is invalid")));
    validate_name(info->table_uuid, "table_uuid");
    validate_name(info->metadata_location, "metadata_location");
    validate_name(info->table_location, "table_location");
    if (info->previous_metadata_location != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("previous_metadata_location must be NULL when registering a table")));
    validate_name(input->schema_json, "schema_json");
    if (input->schema_id < 0 || input->spec_id < 0 || info->last_column_id < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("metadata ids must be non-negative")));
    if (!info->has_current_schema_id || info->current_schema_id != input->schema_id)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("current_schema_id must match schema_id")));
    if (!info->has_default_spec_id || info->default_spec_id != input->spec_id)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("default_spec_id must match spec_id")));

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;
        lock_namespace_for_share(namespace_name);
        insert_table_record(namespace_name, table_name, info);
        insert_schema_fields(info->table_uuid, input->schema_id, input->schema_json);
        insert_partition_spec(info->table_uuid, input->spec_id, input->partition_fields_json);
        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        /* Save the original error before SPI cleanup can overwrite it. */
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata register table");
    }
    PG_END_TRY();
}

/* ------------------------------------------------------------------ */
/*  Free MetaTableInfo                                                 */
/* ------------------------------------------------------------------ */

/*
 * Free a MetaTableInfo and all its palloc'd members.
 */
void
iceberg_meta_free_table_info(MetaTableInfo *info)
{
    if (info == NULL)
        return;

    pfree(info->namespace_name);
    pfree(info->table_name);
    pfree(info->table_uuid);
    pfree(info->metadata_location);
    pfree(info->previous_metadata_location);
    pfree(info->table_location);
    pfree(info);
}

/* ------------------------------------------------------------------ */
/*  Table lookup (read-only)                                           */
/* ------------------------------------------------------------------ */

/*
 * Look up a table in the local catalog by namespace and table name.
 * Returns a palloc'd MetaTableInfo if found, or NULL if not found.
 * The caller must free the result with iceberg_meta_free_table_info().
 */
MetaTableInfo *
iceberg_meta_get_table(const char *namespace_name, const char *table_name)
{
    Datum values[2];
    Oid argtypes[2] = {TEXTOID, TEXTOID};
    MetaTableInfo *info = NULL;
    bool spi_connected = false;
    int rc;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");

    values[0] = CStringGetTextDatum(namespace_name);
    values[1] = CStringGetTextDatum(table_name);

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;

        rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
            "SELECT "
            "    relid, namespace, table_name, table_uuid,"
            "    metadata_location, previous_metadata_location, table_location,"
            "    last_column_id, current_schema_id, current_snapshot_id, default_spec_id "
            "FROM iceberg_catalog.tables_internal "
            "WHERE namespace = $1 "
            "  AND table_name = $2",
            2,
            argtypes,
            values,
            NULL,
            true,
            1);

        if (rc != SPI_OK_SELECT)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("metadata table lookup query failed")));

        if (SPI_processed > 0)
        {
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            HeapTuple row = SPI_tuptable->vals[0];
            bool isnull;
            char *v;

            info = (MetaTableInfo *) palloc0(sizeof(MetaTableInfo));

            info->relid = DatumGetObjectId(SPI_getbinval(row, tupdesc, 1, &isnull));
            v = SPI_getvalue(row, tupdesc, 2);
            info->namespace_name = v ? pstrdup(v) : pstrdup("");
            v = SPI_getvalue(row, tupdesc, 3);
            info->table_name = v ? pstrdup(v) : pstrdup("");
            v = SPI_getvalue(row, tupdesc, 4);
            info->table_uuid = v ? pstrdup(v) : pstrdup("");
            v = SPI_getvalue(row, tupdesc, 5);
            info->metadata_location = v ? pstrdup(v) : pstrdup("");

            /* previous_metadata_location is nullable */
            v = SPI_getvalue(row, tupdesc, 6);
            info->previous_metadata_location = v ? pstrdup(v) : NULL;

            v = SPI_getvalue(row, tupdesc, 7);
            info->table_location = v ? pstrdup(v) : pstrdup("");

            info->last_column_id = DatumGetInt32(SPI_getbinval(row, tupdesc, 8, &isnull));

            info->current_schema_id = DatumGetInt32(SPI_getbinval(row, tupdesc, 9, &isnull));
            info->has_current_schema_id = true;

            /* current_snapshot_id is nullable */
            info->current_snapshot_id = DatumGetInt64(SPI_getbinval(row, tupdesc, 10, &isnull));
            info->has_current_snapshot_id = !isnull;

            info->default_spec_id = DatumGetInt32(SPI_getbinval(row, tupdesc, 11, &isnull));
            info->has_default_spec_id = true;
        }

        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        /* Save the original error before SPI cleanup can overwrite it. */
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata table lookup");
    }
    PG_END_TRY();

    return info;
}

/* ------------------------------------------------------------------ */
/*  List tables                                                        */
/* ------------------------------------------------------------------ */

/*
 * List tables within the given namespace using cursor-based pagination.
 *
 * The page_token is the last table_name from the previous page, used as
 * a keyset cursor: we select rows with table_name > page_token, ordered
 * ascending, limited to page_size.  If the result set is exactly page_size
 * rows long there may be more pages, so next-page-token is set to the last
 * table_name; otherwise it is null.
 */
char *
iceberg_meta_list_tables(const char *namespace_name,
                         int page_size,
                         const char *page_token)
{
    Datum values[3];
    Oid argtypes[3] = {TEXTOID, TEXTOID, INT4OID};
    char nulls[3] = {' ', ' ', ' '};
    int rc;
    bool spi_connected = false;
    char *result = NULL;

    validate_name(namespace_name, "namespace_name");

    if (page_size < 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("page_size must be >= 1")));

    /* Keyset cursor: $2 is the previous page's last table_name, or NULL. */
    values[0] = CStringGetTextDatum(namespace_name);
    values[1] = page_token != NULL ? CStringGetTextDatum(page_token) : (Datum) 0;
    values[2] = Int32GetDatum(page_size);
    if (page_token == NULL)
        nulls[1] = 'n';

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;

        rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
            "SELECT table_name "
            "FROM iceberg_catalog.tables_internal "
            "WHERE namespace = $1 "
            "  AND ($2::text IS NULL OR table_name > $2::text) "
            "ORDER BY table_name ASC "
            "LIMIT $3",
            3,
            argtypes,
            values,
            nulls,
            true,
            page_size);
        if (rc != SPI_OK_SELECT)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("metadata list tables query failed")));

        {
            StringInfoData buf;
            uint64 i;

            initStringInfo(&buf);

            appendStringInfoString(&buf, "{\"identifiers\":[");

            for (i = 0; i < SPI_processed; i++)
            {
                if (i > 0)
                    appendStringInfoString(&buf, ",");

                {
                    char *name = SPI_getvalue(SPI_tuptable->vals[i],
                                              SPI_tuptable->tupdesc, 1);
                    if (name == NULL)
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("table_name is NULL in tables_internal")));

                    appendStringInfo(&buf,
                                     "{\"namespace\":[\"%s\"],\"name\":\"%s\"}",
                                     namespace_name, name);
                    pfree(name);
                }
            }

            appendStringInfoString(&buf, "],");

            if (SPI_processed >= (uint64) page_size)
            {
                char *last_name = SPI_getvalue(
                    SPI_tuptable->vals[SPI_processed - 1],
                    SPI_tuptable->tupdesc, 1);
                if (last_name == NULL)
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("table_name is NULL in tables_internal")));

                appendStringInfo(&buf, "\"next-page-token\":\"%s\"", last_name);
                pfree(last_name);
            }
            else
            {
                appendStringInfoString(&buf, "\"next-page-token\":null");
            }

            appendStringInfoString(&buf, "}");
            result = buf.data;
            /* StringInfoData.data is palloc'd; we transfer ownership to result. */
        }

        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata list tables query");
    }
    PG_END_TRY();

    return result;
}

/* ------------------------------------------------------------------ */
/*  Internal SPI helpers (assume SPI already connected)                 */
/* ------------------------------------------------------------------ */

/*
 * SELECT ... FOR UPDATE on a single table row.
 * Returns a palloc'd MetaTableInfo, or NULL if no matching row exists.
 * Assumes SPI is already connected.
 */
static MetaTableInfo *
get_table_for_update(const char *namespace_name, const char *table_name)
{
    Datum values[2];
    Oid argtypes[2] = {TEXTOID, TEXTOID};
    int rc;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");

    values[0] = CStringGetTextDatum(namespace_name);
    values[1] = CStringGetTextDatum(table_name);

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "SELECT relid, namespace, table_name, table_uuid,"
        "       metadata_location, previous_metadata_location, table_location,"
        "       last_column_id, current_schema_id, current_snapshot_id, default_spec_id "
        "FROM iceberg_catalog.tables_internal "
        "WHERE namespace = $1 "
        "  AND table_name = $2 "
        "FOR UPDATE",
        2,
        argtypes,
        values,
        NULL,
        false,
        1);
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to lock table metadata row")));

    if (SPI_processed == 0)
        return NULL;

    {
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        HeapTuple tuple = SPI_tuptable->vals[0];
        MetaTableInfo *info;
        char *val;
        bool isnull;

        info = (MetaTableInfo *) palloc0(sizeof(MetaTableInfo));

        info->relid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 1, &isnull));

        val = SPI_getvalue(tuple, tupdesc, 2);
        info->namespace_name = val ? pstrdup(val) : pstrdup("");

        val = SPI_getvalue(tuple, tupdesc, 3);
        info->table_name = val ? pstrdup(val) : pstrdup("");

        val = SPI_getvalue(tuple, tupdesc, 4);
        info->table_uuid = val ? pstrdup(val) : pstrdup("");

        val = SPI_getvalue(tuple, tupdesc, 5);
        info->metadata_location = val ? pstrdup(val) : pstrdup("");

        val = SPI_getvalue(tuple, tupdesc, 6);
        info->previous_metadata_location = val ? pstrdup(val) : NULL;

        val = SPI_getvalue(tuple, tupdesc, 7);
        info->table_location = val ? pstrdup(val) : pstrdup("");

        info->last_column_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 8, &isnull));

        info->current_schema_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        info->has_current_schema_id = !isnull;

        info->current_snapshot_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 10, &isnull));
        info->has_current_snapshot_id = !isnull;

        info->default_spec_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 11, &isnull));
        info->has_default_spec_id = !isnull;

        return info;
    }
}

/* ------------------------------------------------------------------ */
/*  Rename table  (internal + public)                                   */
/* ------------------------------------------------------------------ */

/*
 * Internal rename implementation.  Assumes SPI is already connected.
 *
 * Per design doc section 6.2.15:
 *   1. Lock the destination namespace (FOR SHARE).
 *   2. Lock the source table row (FOR UPDATE) and verify it exists.
 *   3. Verify no table already exists at the destination.
 *   4. UPDATE the row with the new namespace/table_name.
 */
static void
iceberg_meta_rename_table(const char *src_ns, const char *src_table,
                          const char *dst_ns, const char *dst_table)
{
    Datum values[4];
    Oid argtypes[4] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID};
    MetaTableInfo *info;
    int rc;

    validate_name(src_ns, "src_ns");
    validate_name(src_table, "src_table");
    validate_name(dst_ns, "dst_ns");
    validate_name(dst_table, "dst_table");

    /* 1. Lock the destination namespace (FOR SHARE) */
    values[0] = CStringGetTextDatum(dst_ns);
    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "SELECT 1 "
        "FROM iceberg_catalog.namespaces "
        "WHERE catalog_name = current_database()::text "
        "  AND namespace = $1 "
        "FOR SHARE",
        1,
        argtypes,
        values,
        NULL,
        false,
        1);
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to lock destination namespace")));
    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("destination namespace not found")));

    /* 2. Lock the source table (FOR UPDATE) and confirm it exists */
    info = get_table_for_update(src_ns, src_table);
    if (info == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("source table not found")));
    iceberg_meta_free_table_info(info);

    /* 3. Verify no table already exists at the destination */
    values[0] = CStringGetTextDatum(dst_ns);
    values[1] = CStringGetTextDatum(dst_table);
    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "SELECT 1 "
        "FROM iceberg_catalog.tables_internal "
        "WHERE namespace = $1 "
        "  AND table_name = $2",
        2,
        argtypes,
        values,
        NULL,
        true,
        1);
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to check destination table existence")));
    if (SPI_processed > 0)
        ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg("destination table already exists")));

    /* 4. Perform the rename (UPDATE) */
    values[0] = CStringGetTextDatum(src_ns);
    values[1] = CStringGetTextDatum(src_table);
    values[2] = CStringGetTextDatum(dst_ns);
    values[3] = CStringGetTextDatum(dst_table);
    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "UPDATE iceberg_catalog.tables_internal "
        "SET namespace = $3, table_name = $4 "
        "WHERE namespace = $1 "
        "  AND table_name = $2",
        4,
        argtypes,
        values,
        NULL,
        false,
        0);
    if (rc != SPI_OK_UPDATE)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to rename table metadata")));

    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("source table disappeared during rename")));
}

/*
 * Rename a table in the local metadata tables (service wrapper).
 *
 * Connects SPI, performs the rename (including preconditions), and
 * finishes SPI.  Errors are translated via the internal
 * throw_translated_spi_error pattern.
 */
void
iceberg_meta_rename_table_record(const char *src_ns, const char *src_table,
                                 const char *dst_ns, const char *dst_table)
{
    bool spi_connected = false;

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;
        iceberg_meta_rename_table(src_ns, src_table, dst_ns, dst_table);
        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata rename table");
    }
    PG_END_TRY();
}

/* ------------------------------------------------------------------ */
/*  Table deletion  (internal + public)                                 */
/* ------------------------------------------------------------------ */

/*
 * Internal: DELETE a table record from iceberg_catalog.tables_internal.
 *
 * Raises ERRCODE_UNDEFINED_OBJECT if no row matched.  Assumes SPI is
 * already connected.
 */
static void
iceberg_meta_delete_table(const char *namespace_name,
                          const char *table_name)
{
    Datum values[2];
    Oid argtypes[2] = {TEXTOID, TEXTOID};
    int rc;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");

    values[0] = CStringGetTextDatum(namespace_name);
    values[1] = CStringGetTextDatum(table_name);

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "DELETE FROM iceberg_catalog.tables_internal "
        "WHERE namespace = $1 "
        "  AND table_name = $2",
        2,
        argtypes,
        values,
        NULL,
        false,
        0);
    if (rc != SPI_OK_DELETE)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to delete table metadata")));

    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("table not found")));
}

/*
 * Lock an Iceberg table row for update and return its metadata.
 *
 * Service function: manages its own SPI connect/finish.
 * Raises ERRCODE_UNDEFINED_OBJECT if the table does not exist.
 */
MetaTableInfo *
iceberg_meta_lock_table(const char *namespace_name,
                        const char *table_name)
{
    MetaTableInfo *info = NULL;
    bool spi_connected = false;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;
        info = get_table_for_update(namespace_name, table_name);
        if (info == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("table not found")));
        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata lock table");
    }
    PG_END_TRY();

    return info;
}

/*
 * Delete the table record from iceberg_catalog.tables_internal.
 *
 * Service function: manages its own SPI connect/finish.
 * Raises ERRCODE_UNDEFINED_OBJECT if no matching row is found.
 */
void
iceberg_meta_drop_table_record(const char *namespace_name,
                               const char *table_name)
{
    bool spi_connected = false;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;
        iceberg_meta_delete_table(namespace_name, table_name);
        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata drop table record");
    }
    PG_END_TRY();
}

/* ------------------------------------------------------------------ */
/*  Table update / lock operations                                     */
/* ------------------------------------------------------------------ */

/*
 * Lock a table row for write-path operations (SELECT ... FOR UPDATE).
 * Returns a palloc'd MetaTableInfo; caller must free via iceberg_meta_free_table_info.
 * Returns NULL if the table does not exist.
 */
MetaTableInfo*
iceberg_meta_get_table_for_update(const char *namespace_name, const char *table_name)
{
    Datum values[2];
    Oid argtypes[2] = {TEXTOID, TEXTOID};
    bool spi_connected = false;
    int rc;
    MetaTableInfo *info = NULL;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");

    values[0] = CStringGetTextDatum(namespace_name);
    values[1] = CStringGetTextDatum(table_name);

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;

        rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
            "SELECT relid::oid, namespace, table_name, table_uuid::text,"
            "       metadata_location, previous_metadata_location, table_location,"
            "       last_column_id, current_schema_id, current_snapshot_id, default_spec_id "
            "FROM iceberg_catalog.tables_internal "
            "WHERE namespace = $1 AND table_name = $2 "
            "FOR UPDATE",
            2, argtypes, values, NULL, false, 1);

        if (rc != SPI_OK_SELECT)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("metadata get table for update query failed")));

        if (SPI_processed > 0)
        {
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            HeapTuple tuple = SPI_tuptable->vals[0];
            bool isnull;
            char *val;

            info = (MetaTableInfo *) palloc0(sizeof(MetaTableInfo));

            /* col 1: relid (oid) */
            info->relid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 1, &isnull));

            /* col 2: namespace */
            val = SPI_getvalue(tuple, tupdesc, 2);
            info->namespace_name = val ? pstrdup(val) : pstrdup("");

            /* col 3: table_name */
            val = SPI_getvalue(tuple, tupdesc, 3);
            info->table_name = val ? pstrdup(val) : pstrdup("");

            /* col 4: table_uuid::text */
            val = SPI_getvalue(tuple, tupdesc, 4);
            info->table_uuid = val ? pstrdup(val) : pstrdup("");

            /* col 5: metadata_location */
            val = SPI_getvalue(tuple, tupdesc, 5);
            info->metadata_location = val ? pstrdup(val) : NULL;

            /* col 6: previous_metadata_location */
            val = SPI_getvalue(tuple, tupdesc, 6);
            info->previous_metadata_location = val ? pstrdup(val) : NULL;

            /* col 7: table_location */
            val = SPI_getvalue(tuple, tupdesc, 7);
            info->table_location = val ? pstrdup(val) : NULL;

            /* col 8: last_column_id */
            info->last_column_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 8, &isnull));

            /* col 9: current_schema_id */
            info->current_schema_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 9, &isnull));
            info->has_current_schema_id = !isnull;

            /* col 10: current_snapshot_id */
            info->current_snapshot_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 10, &isnull));
            info->has_current_snapshot_id = !isnull;

            /* col 11: default_spec_id */
            info->default_spec_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 11, &isnull));
            info->has_default_spec_id = !isnull;
        }

        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata get table for update");
    }
    PG_END_TRY();

    return info;
}

/*
 * Update table metadata pointers and optional summary fields with optimistic locking.
 * Uses CAS: WHERE metadata_location = old AND table_uuid check.
 *
 * This is an internal function; SQL functions should use the scene-level
 * commit wrappers instead.
 */
void
iceberg_meta_update_table(const char *ns, const char *tbl,
                          const char *uuid, const char *old_meta, const char *new_meta,
                          int64_t new_snap_id, bool has_new_snap,
                          int new_schema_id, bool has_new_schema,
                          int new_last_col_id, bool has_new_last_col,
                          int new_def_spec_id, bool has_new_def_spec)
{
    Datum values[13];
    Oid argtypes[13];
    char nulls[13];
    int rc;

    validate_name(ns, "namespace_name");
    validate_name(tbl, "table_name");
    validate_name(uuid, "table_uuid");
    validate_name(old_meta, "old_metadata_location");
    validate_name(new_meta, "new_metadata_location");

    argtypes[0] = TEXTOID;
    argtypes[1] = TEXTOID;
    argtypes[2] = TEXTOID;
    argtypes[3] = TEXTOID;
    argtypes[4] = INT4OID;
    argtypes[5] = INT8OID;
    argtypes[6] = INT4OID;
    argtypes[7] = INT4OID;
    argtypes[8] = INT4OID;
    argtypes[9] = INT4OID;
    argtypes[10] = INT4OID;
    argtypes[11] = INT4OID;
    argtypes[12] = TEXTOID;

    values[0] = CStringGetTextDatum(ns);
    values[1] = CStringGetTextDatum(tbl);
    values[2] = CStringGetTextDatum(uuid);
    values[3] = CStringGetTextDatum(new_meta);
    values[4] = Int32GetDatum(has_new_snap ? 1 : 0);
    values[5] = Int64GetDatum(new_snap_id);
    values[6] = Int32GetDatum(has_new_schema ? 1 : 0);
    values[7] = Int32GetDatum(new_schema_id);
    values[8] = Int32GetDatum(has_new_last_col ? 1 : 0);
    values[9] = Int32GetDatum(new_last_col_id);
    values[10] = Int32GetDatum(has_new_def_spec ? 1 : 0);
    values[11] = Int32GetDatum(new_def_spec_id);
    values[12] = CStringGetTextDatum(old_meta);

    /* No NULLs in bind values */
    memset(nulls, ' ', sizeof(nulls));

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "UPDATE iceberg_catalog.tables_internal "
        "SET previous_metadata_location = metadata_location, "
        "    metadata_location = $4, "
        "    current_snapshot_id = CASE WHEN $5::int <> 0 THEN $6::bigint ELSE current_snapshot_id END, "
        "    current_schema_id   = CASE WHEN $7::int <> 0 THEN $8::int   ELSE current_schema_id   END, "
        "    last_column_id      = CASE WHEN $9::int <> 0 THEN $10::int  ELSE last_column_id      END, "
        "    default_spec_id     = CASE WHEN $11::int <> 0 THEN $12::int ELSE default_spec_id     END "
        "WHERE namespace = $1 AND table_name = $2 "
        "  AND table_uuid = $3::uuid "
        "  AND metadata_location = $13 "
        "RETURNING table_uuid::text",
        13, argtypes, values, nulls, false, 1);

    if (rc != SPI_OK_UPDATE_RETURNING)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("metadata update table query failed")));

    if (SPI_processed > 0)
        return;  /* success */

    /*
     * Zero rows updated -- diagnostic phase.  Check whether the
     * table still exists and if the UUID matches.
     */
    {
        Datum diag_values[2];
        Oid diag_argtypes[2] = {TEXTOID, TEXTOID};
        int diag_rc;

        diag_values[0] = CStringGetTextDatum(ns);
        diag_values[1] = CStringGetTextDatum(tbl);

        diag_rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
            "SELECT table_uuid::text, metadata_location "
            "FROM iceberg_catalog.tables_internal "
            "WHERE namespace = $1 AND table_name = $2",
            2, diag_argtypes, diag_values, NULL, true, 1);

        if (diag_rc != SPI_OK_SELECT)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("metadata update table diagnostic query failed")));

        if (SPI_processed == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("table \"%s.%s\" does not exist", ns, tbl)));

        {
            char *existing_uuid = SPI_getvalue(SPI_tuptable->vals[0],
                                                SPI_tuptable->tupdesc, 1);
            char *existing_meta = SPI_getvalue(SPI_tuptable->vals[0],
                                                SPI_tuptable->tupdesc, 2);

            if (existing_uuid == NULL || strcmp(existing_uuid, uuid) != 0)
                ereport(ERROR,
                        (errcode(ERRCODE_DUPLICATE_OBJECT),
                         errmsg("table \"%s.%s\" UUID has changed since lock was acquired", ns, tbl)));

            if (existing_meta == NULL || strcmp(existing_meta, old_meta) != 0)
                ereport(ERROR,
                        (errcode(ERRCODE_DUPLICATE_OBJECT),
                         errmsg("table \"%s.%s\" metadata_location changed concurrently", ns, tbl)));

            /* Both UUID and metadata_location match -- should never happen */
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("table \"%s.%s\" update failed for unknown reason", ns, tbl)));
        }
    }
}

/*
 * Insert a snapshot summary row into iceberg_catalog.snapshots.
 * Internal function; does not manage SPI.
 */
void
iceberg_meta_insert_snapshot(const char *table_uuid,
                              int64_t snapshot_id,
                              int schema_id,
                              bool has_schema_id,
                              int64_t timestamp_ms,
                              const char *manifest_list,
                              int64_t total_records,
                              bool has_total_records)
{
    Datum values[6];
    Oid argtypes[6] = {TEXTOID, INT8OID, INT4OID, INT8OID, TEXTOID, INT8OID};
    char nulls[6] = {' ', ' ', ' ', ' ', ' ', ' '};
    int rc;

    validate_name(table_uuid, "table_uuid");

    values[0] = CStringGetTextDatum(table_uuid);
    values[1] = Int64GetDatum(snapshot_id);
    values[2] = Int32GetDatum(schema_id);
    values[3] = Int64GetDatum(timestamp_ms);
    if (manifest_list != NULL)
        values[4] = CStringGetTextDatum(manifest_list);
    else
        nulls[4] = 'n';
    values[5] = Int64GetDatum(total_records);

    if (!has_schema_id)
        nulls[2] = 'n';
    if (!has_total_records)
        nulls[5] = 'n';

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "INSERT INTO iceberg_catalog.snapshots("
        "    table_uuid, snapshot_id, schema_id, timestamp_ms,"
        "    manifest_list, total_records"
        ") VALUES ("
        "    $1::uuid, $2, $3, $4,"
        "    $5, $6"
        ")",
        6, argtypes, values, nulls, false, 0);

    if (rc != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to insert snapshot metadata")));

    if (SPI_processed != 1)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("unexpected snapshot insert count")));
}

/*
 * Scene-level commit: update table pointer + insert snapshot cache.
 * This is the primary entry-point for commit_table.
 */
void
iceberg_meta_commit_table(const MetaCommitTableInput *input)
{
    bool spi_connected = false;

    if (input == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("input is required")));

    validate_name(input->namespace_name, "namespace_name");
    validate_name(input->table_name, "table_name");
    validate_name(input->table_uuid, "table_uuid");
    validate_name(input->old_metadata_location, "old_metadata_location");
    validate_name(input->new_metadata_location, "new_metadata_location");

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;

        iceberg_meta_update_table(input->namespace_name, input->table_name,
            input->table_uuid, input->old_metadata_location, input->new_metadata_location,
            input->new_snapshot_id, true,   /* always update snapshot_id */
            0, false,                       /* don't update schema_id */
            0, false,                       /* don't update last_column_id */
            0, false);                      /* don't update default_spec_id */

        iceberg_meta_insert_snapshot(input->table_uuid, input->new_snapshot_id,
            input->snapshot_schema_id, input->has_snapshot_schema_id,
            input->snapshot_timestamp_ms, input->manifest_list,
            input->total_records, input->has_total_records);

        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata commit table");
    }
    PG_END_TRY();
}

/*
 * Scene-level schema change commit: update table pointer + insert schema cache.
 * This is the primary entry-point for add_column.
 */
void
iceberg_meta_commit_schema_change(const MetaCommitSchemaChangeInput *input)
{
    bool spi_connected = false;

    if (input == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("input is required")));

    validate_name(input->namespace_name, "namespace_name");
    validate_name(input->table_name, "table_name");
    validate_name(input->table_uuid, "table_uuid");
    validate_name(input->old_metadata_location, "old_metadata_location");
    validate_name(input->new_metadata_location, "new_metadata_location");
    validate_name(input->schema_json, "schema_json");

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;

        iceberg_meta_update_table(input->namespace_name, input->table_name,
            input->table_uuid, input->old_metadata_location, input->new_metadata_location,
            0, false,                      /* don't update snapshot_id */
            input->new_schema_id, true,    /* update schema_id */
            input->new_last_column_id, true, /* update last_column_id */
            0, false);                     /* don't update default_spec_id */

        insert_schema_fields(input->table_uuid, input->new_schema_id, input->schema_json);

        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata commit schema change");
    }
    PG_END_TRY();
}
