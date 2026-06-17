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
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

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

static MetaTableInfo *
copy_table_info_from_tuple(HeapTuple tuple, TupleDesc tupdesc, MemoryContext target_context)
{
    MetaTableInfo *info;
    MemoryContext old_context;
    bool isnull;
    Datum value;

    old_context = MemoryContextSwitchTo(target_context);

    info = (MetaTableInfo *) palloc0(sizeof(MetaTableInfo));

    value = SPI_getbinval(tuple, tupdesc, 1, &isnull);
    info->relid = isnull ? InvalidOid : DatumGetObjectId(value);
    info->namespace_name = SPI_getvalue(tuple, tupdesc, 2);
    info->table_name = SPI_getvalue(tuple, tupdesc, 3);
    info->table_uuid = SPI_getvalue(tuple, tupdesc, 4);
    info->metadata_location = SPI_getvalue(tuple, tupdesc, 5);
    info->previous_metadata_location = SPI_getvalue(tuple, tupdesc, 6);
    info->table_location = SPI_getvalue(tuple, tupdesc, 7);

    value = SPI_getbinval(tuple, tupdesc, 8, &isnull);
    info->last_column_id = isnull ? 0 : DatumGetInt32(value);

    value = SPI_getbinval(tuple, tupdesc, 9, &isnull);
    info->has_current_schema_id = !isnull;
    info->current_schema_id = isnull ? 0 : DatumGetInt32(value);

    value = SPI_getbinval(tuple, tupdesc, 10, &isnull);
    info->has_current_snapshot_id = !isnull;
    info->current_snapshot_id = isnull ? 0 : DatumGetInt64(value);

    value = SPI_getbinval(tuple, tupdesc, 11, &isnull);
    info->has_default_spec_id = !isnull;
    info->default_spec_id = isnull ? 0 : DatumGetInt32(value);

    MemoryContextSwitchTo(old_context);

    return info;
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

/*
 * Read the table head record from iceberg_catalog.tables_internal.
 * Returns NULL if no matching table exists.
 */
MetaTableInfo *
iceberg_meta_get_table(const char *namespace_name, const char *table_name)
{
    Datum values[2];
    Oid argtypes[2] = {TEXTOID, TEXTOID};
    MetaTableInfo *info = NULL;
    MemoryContext caller_context = CurrentMemoryContext;
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
            "    relid, namespace, table_name, table_uuid::text,"
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
                     errmsg("metadata get table query failed")));

        if (SPI_processed > 0)
            info = copy_table_info_from_tuple(SPI_tuptable->vals[0],
                                              SPI_tuptable->tupdesc,
                                              caller_context);

        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata get table query");
    }
    PG_END_TRY();

    return info;
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
/*  Public API                                                         */
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
/*  Drop table helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * Delete a table row from tables_internal (no SPI management).
 */
static void
iceberg_meta_delete_table(const char *ns, const char *tbl)
{
    Datum v[2] = {CStringGetTextDatum(ns), CStringGetTextDatum(tbl)};
    Oid t[2] = {TEXTOID, TEXTOID};
    int rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "DELETE FROM iceberg_catalog.tables_internal WHERE namespace = $1 AND table_name = $2",
        2, t, v, NULL, false, 0);
    if (rc != SPI_OK_DELETE)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to delete table metadata")));
    if (SPI_processed == 0)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("table not found")));
}

/*
 * Delete a table from the local metadata tables (service wrapper).
 *
 * Connects SPI, deletes the table head row from tables_internal, and
 * relies on ON DELETE CASCADE for dependent rows.
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
/*  Rename table                                                       */
/* ------------------------------------------------------------------ */

/*
 * Internal rename implementation.  Assumes SPI is already connected.
 *
 * Per design doc section 7.9:
 *   1. Lock the destination namespace (FOR SHARE).
 *   2. UPDATE the source row with the new namespace/table_name.
 *   3. Use UPDATE row count and the primary key constraint to detect
 *      source-missing and destination-conflict cases.
 */
static void
iceberg_meta_rename_table(const char *src_ns, const char *src_table,
                          const char *dst_ns, const char *dst_table)
{
    Oid text_arg[4] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID};
    int rc;

    validate_name(src_ns, "src_ns");
    validate_name(src_table, "src_table");
    validate_name(dst_ns, "dst_ns");
    validate_name(dst_table, "dst_table");

    /* 1. Lock the destination namespace (FOR SHARE) */
    {
        Datum v[1] = {CStringGetTextDatum(dst_ns)};
        rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
            "SELECT 1 "
            "FROM iceberg_catalog.namespaces "
            "WHERE catalog_name = current_database()::text "
            "  AND namespace = $1 "
            "FOR SHARE",
            1, text_arg, v, NULL, false, 1);
    }
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to lock destination namespace")));
    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("destination namespace not found")));

    /* 2. Perform the rename (UPDATE). The PK handles destination conflicts. */
    {
        Datum v[4] = {CStringGetTextDatum(src_ns), CStringGetTextDatum(src_table),
                      CStringGetTextDatum(dst_ns), CStringGetTextDatum(dst_table)};
        rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
            "UPDATE iceberg_catalog.tables_internal "
            "SET namespace = $3, table_name = $4 "
            "WHERE namespace = $1 "
            "  AND table_name = $2",
            4, text_arg, v, NULL, false, 0);
    }
    if (rc != SPI_OK_UPDATE)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to rename table metadata")));

    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("source table not found")));
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
        if (edata->sqlerrcode == ERRCODE_UNIQUE_VIOLATION) {
            FreeErrorData(edata);
            FlushErrorState();
            ereport(ERROR,
                    (errcode(ERRCODE_DUPLICATE_OBJECT),
                     errmsg("destination table already exists")));
        }
        throw_translated_spi_error(edata, "metadata rename table");
    }
    PG_END_TRY();
}

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
/*  Namespace creation                                                 */
/* ------------------------------------------------------------------ */

/*
 * Create a namespace in the local catalog.
 *
 * Validates the namespace name and properties JSON, then inserts a row
 * into iceberg_catalog.namespaces.  Raises ERRCODE_DUPLICATE_OBJECT
 * if the namespace already exists.
 */
void
iceberg_meta_create_namespace(const char *namespace_name,
                               const char *properties_json)
{
    Datum values[2];
    Oid argtypes[2] = {TEXTOID, TEXTOID};
    const char *props;
    int rc;
    bool spi_connected = false;

    validate_name(namespace_name, "namespace_name");

    props = (properties_json == NULL) ? "{}" : properties_json;

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;

        /* Validate properties is a JSON object */
        {
            Datum chk[1] = {CStringGetTextDatum(props)};
            Oid chk_type[1] = {TEXTOID};
            rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
                "SELECT jsonb_typeof($1::jsonb) = 'object'",
                1, chk_type, chk, NULL, true, 1);
            if (rc != SPI_OK_SELECT || SPI_processed != 1)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("failed to validate namespace properties JSON")));

            {
                bool isnull;
                bool is_object = DatumGetBool(
                    SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
                if (!is_object)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("namespace properties must be a JSON object")));
            }
        }

        /* Insert the namespace row */
        values[0] = CStringGetTextDatum(namespace_name);
        values[1] = CStringGetTextDatum(props);

        rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
            "INSERT INTO iceberg_catalog.namespaces("
            "    catalog_name, namespace, properties"
            ") VALUES ("
            "    current_database()::text, $1, $2::jsonb"
            ")",
            2, argtypes, values, NULL, false, 0);
        if (rc != SPI_OK_INSERT)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("failed to insert namespace metadata")));

        if (SPI_processed != 1)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("unexpected namespace insert count")));

        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        throw_translated_spi_error(edata, "metadata create namespace");
    }
    PG_END_TRY();
}

/* ------------------------------------------------------------------ */
/*  Page-token encoding/decoding (internal helpers)                    */
/* ------------------------------------------------------------------ */

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *
b64_encode(const char *src, int srclen)
{
    int outlen = ((srclen + 2) / 3) * 4;
    char *out;
    int i, j;

    out = (char *) palloc(outlen + 1);
    for (i = 0, j = 0; i < srclen; i += 3)
    {
        unsigned int v = ((unsigned char) src[i]) << 16;
        v |= (i + 1 < srclen) ? ((unsigned char) src[i + 1]) << 8 : 0;
        v |= (i + 2 < srclen) ? ((unsigned char) src[i + 2]) : 0;

        out[j++] = B64_ALPHABET[(v >> 18) & 0x3F];
        out[j++] = B64_ALPHABET[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < srclen) ? B64_ALPHABET[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < srclen) ? B64_ALPHABET[v & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
}

static int
b64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/*
 * Decode a base64 string.  Output is palloc'd and null-terminated.
 * Returns NULL on invalid input.
 */
static char *
b64_decode(const char *src, int srclen, int *outlen)
{
    int i, j;
    char *out;

    if (srclen % 4 != 0)
        return NULL;

    while (srclen > 0 && src[srclen - 1] == '=')
        srclen--;

    out = (char *) palloc((srclen / 4) * 3 + 1);

    for (i = 0, j = 0; i < srclen; i += 4)
    {
        int a = b64_decode_char(src[i]);
        int b = b64_decode_char(src[i + 1]);
        int c = (src[i + 2] == '=') ? 0 : b64_decode_char(src[i + 2]);
        int d = (src[i + 3] == '=') ? 0 : b64_decode_char(src[i + 3]);

        if (a < 0 || b < 0 || (src[i + 2] != '=' && c < 0) || (src[i + 3] != '=' && d < 0))
        {
            pfree(out);
            return NULL;
        }

        out[j++] = (char) ((a << 2) | (b >> 4));
        if (src[i + 2] != '=')
            out[j++] = (char) ((b << 4) | (c >> 2));
        if (src[i + 3] != '=')
            out[j++] = (char) ((c << 6) | d);
    }
    out[j] = '\0';
    *outlen = j;
    return out;
}

/*
 * Decode a page_token (base64-encoded JSON) and extract the "last" field.
 * Token format: {"v":1,"type":"table","namespace":"<ns>","last":"<name>"}
 * Returns a palloc'd copy of the "last" value (empty string for first page),
 * or NULL on malformed input.
 */
static char *
page_token_decode_last(const char *page_token)
{
    char *json;
    int jsonlen;
    const char *key = "\"last\":\"";
    const char *pos, *end;

    if (page_token == NULL || page_token[0] == '\0')
        return pstrdup("");

    json = b64_decode(page_token, (int) strlen(page_token), &jsonlen);
    if (json == NULL)
        return NULL;

    pos = strstr(json, key);
    if (pos == NULL)
    {
        pfree(json);
        return NULL;
    }
    pos += strlen(key);

    end = strchr(pos, '"');
    if (end == NULL)
    {
        pfree(json);
        return NULL;
    }

    {
        int len = (int) (end - pos);
        char *result = (char *) palloc(len + 1);
        memcpy(result, pos, len);
        result[len] = '\0';
        pfree(json);
        return result;
    }
}

/*
 * Encode a next-page-token from namespace and last table name.
 * Token format (pre-base64): {"v":1,"type":"table","namespace":"<ns>","last":"<name>"}
 */
static char *
page_token_encode_next(const char *namespace_name, const char *last_table)
{
    StringInfoData buf;
    char *encoded;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "{\"v\":1,\"type\":\"table\",\"namespace\":\"%s\",\"last\":\"%s\"}",
        namespace_name, last_table);
    encoded = b64_encode(buf.data, buf.len);
    pfree(buf.data);
    return encoded;
}

/*
 * Validate that a page_token can be decoded and has the expected structure.
 * Returns NULL on success, or an error message string on failure.
 * Empty/NULL token is valid (first page).
 */
static const char *
page_token_validate(const char *page_token)
{
    char *json;
    int jsonlen;

    if (page_token == NULL || page_token[0] == '\0')
        return NULL;

    json = b64_decode(page_token, (int) strlen(page_token), &jsonlen);
    if (json == NULL)
        return "page_token is not a valid base64-encoded string";

    if (strstr(json, "\"last\":") == NULL)
    {
        pfree(json);
        return "page_token missing required \"last\" field";
    }

    pfree(json);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  List tables                                                        */
/* ------------------------------------------------------------------ */

/*
 * List tables in a namespace with last-key cursor pagination.
 *
 * Queries tables_internal, ordered by table_name ASC.  A page_token
 * encodes the last table_name seen; decoding it yields the start-after
 * cursor.  The result JSON includes an opaque next-page-token when
 * more tables exist beyond the requested page_size.
 *
 * Returns a palloc'd JSON string; caller must pfree().
 */
char *
iceberg_meta_list_tables(const char *namespace_name,
                          int page_size,
                          const char *page_token)
{
    StringInfoData result;
    MemoryContext caller_context = CurrentMemoryContext;
    char *last_table_cursor;
    const char *token_err;
    bool spi_connected = false;
    int total;
    int i;

    /* 1. Validate parameters */
    validate_name(namespace_name, "namespace_name");
    if (page_size < 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("page_size must be >= 1")));

    token_err = page_token_validate(page_token);
    if (token_err != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s", token_err)));

    /* 2. Decode page_token to cursor (palloc'd, "" for first page) */
    last_table_cursor = page_token_decode_last(page_token);
    if (last_table_cursor == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("failed to decode page_token")));

    PG_TRY();
    {
        connect_spi();
        spi_connected = true;

        /* 3. Check namespace exists (inline: SPI already connected) */
        {
            Datum ns_val[1] = {CStringGetTextDatum(namespace_name)};
            Oid ns_type[1] = {TEXTOID};
            int ns_rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
                "SELECT 1 FROM iceberg_catalog.namespaces "
                "WHERE catalog_name = current_database()::text AND namespace = $1",
                1, ns_type, ns_val, NULL, true, 1);
            if (ns_rc != SPI_OK_SELECT || SPI_processed == 0)
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_OBJECT),
                         errmsg("namespace \"%s\" does not exist", namespace_name)));
        }

        /* 4. Query tables with pagination (page_size + 1 to detect next page) */
        {
            Datum query_values[3];
            Oid query_types[3] = {TEXTOID, TEXTOID, INT4OID};
            int query_rc;

            query_values[0] = CStringGetTextDatum(namespace_name);
            query_values[1] = CStringGetTextDatum(last_table_cursor);
            query_values[2] = Int32GetDatum(page_size + 1);

            query_rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
                "SELECT table_name "
                "FROM iceberg_catalog.tables_internal "
                "WHERE namespace = $1 "
                "  AND ($2 = '' OR table_name > $2) "
                "ORDER BY table_name ASC "
                "LIMIT $3",
                3,
                query_types,
                query_values,
                NULL,
                true,
                (int64)(page_size + 1));

            if (query_rc != SPI_OK_SELECT)
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("metadata list tables query failed")));

            total = (int) SPI_processed;
        }

        /* 5. Build result JSON */
        initStringInfo(&result);

        appendStringInfoString(&result, "{\"identifiers\":[");
        for (i = 0; i < total && i < page_size; i++)
        {
            char *table_name = SPI_getvalue(SPI_tuptable->vals[i],
                                             SPI_tuptable->tupdesc, 1);

            if (i > 0)
                appendStringInfoChar(&result, ',');

            appendStringInfo(&result,
                "{\"namespace\":[\"%s\"],\"name\":\"%s\"}",
                namespace_name, table_name);
        }
        appendStringInfoChar(&result, ']');

        /* 6. Encode next-page-token if there are more tables */
        if (total > page_size)
        {
            char *last_returned = SPI_getvalue(
                SPI_tuptable->vals[page_size - 1],
                SPI_tuptable->tupdesc, 1);
            char *next_token = page_token_encode_next(namespace_name, last_returned);

            appendStringInfo(&result, ",\"next-page-token\":\"%s\"", next_token);
            pfree(next_token);
        }
        else
        {
            appendStringInfoString(&result, ",\"next-page-token\":null");
        }

        appendStringInfoChar(&result, '}');

        finish_spi();
        spi_connected = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        finish_spi_quietly(&spi_connected);
        if (last_table_cursor != NULL)
            pfree(last_table_cursor);
        throw_translated_spi_error(edata, "metadata list tables");
    }
    PG_END_TRY();

    pfree(last_table_cursor);

    /* Copy result to caller's memory context (SPI context is gone) */
    {
        char *caller_result;
        MemoryContext oldctx = MemoryContextSwitchTo(caller_context);

        caller_result = pstrdup(result.data);
        MemoryContextSwitchTo(oldctx);
        pfree(result.data);
        return caller_result;
    }
}
