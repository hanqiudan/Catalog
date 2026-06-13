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
