/*-------------------------------------------------------------------------
 *
 * namespace.cpp
 *    Iceberg namespace SQL function implementations.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "lib/stringinfo.h"

#include <string.h>

#include "errors.h"
#include "iceberg_catalog.h"
#include "metadata.h"
#include "namespace.h"


/* ---- create_namespace ---- */
PG_FUNCTION_INFO_V1(iceberg_create_namespace);

Datum
iceberg_create_namespace(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace   TEXT    (required)
     *   2. p_properties  JSONB   (optional, default NULL)
     *
     * Returns: JSONB (CreateNamespaceResponse)
     *   {"namespace": ["<namespace>"], "properties": {<key>: <value>, ...}}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */
    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_create_namespace: expected at least 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_properties (optional, default NULL -> treat as empty object {}) */
    Jsonb *p_properties = NULL;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
        p_properties = DatumGetJsonb(PG_GETARG_DATUM(1));

    /* 2. Validate p_namespace */
    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("namespace must not be empty")));

    /* 3. Validate p_properties (if provided, must be a JSONB object) */
    if (p_properties != NULL)
    {
        Datum type_datum = DirectFunctionCall1(jsonb_typeof,
                               JsonbGetDatum(p_properties));
        char *type_str = text_to_cstring(DatumGetTextP(type_datum));

        if (strcmp(type_str, "object") != 0)
            ereport(ERROR,
                    (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                     errmsg("p_properties must be a JSONB object")));
        pfree(type_str);
    }

    /* 4. Serialize properties for InsertNamespace and response */
    char *props_str;
    if (p_properties != NULL)
        props_str = DatumGetCString(
            DirectFunctionCall1(jsonb_out,
                JsonbGetDatum(p_properties)));
    else
        props_str = pstrdup("{}");

    /* 5. META InsertNamespace */
    PG_TRY();
    {
        iceberg_meta_create_namespace(p_namespace, props_str);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        iceberg_err_rethrow_metadata(edata, "create namespace metadata");
    }
    PG_END_TRY();

    /* 6. Construct and return response */
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "{\"namespace\":[\"%s\"],\"properties\":%s}",
        p_namespace, props_str);
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum(buf.data)));
}


/* ---- is_namespace_existed ---- */
PG_FUNCTION_INFO_V1(iceberg_is_namespace_existed);

Datum
iceberg_is_namespace_existed(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace  TEXT  (required)
     *
     * Returns: JSONB
     *   {"exists": true}   — namespace exists
     *   {"exists": false}  — namespace does not exist (no exception thrown)
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */
    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_is_namespace_existed: expected 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* 2. Validate required parameters */
    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("namespace must not be empty")));

    /* 3. Check namespace existence via META */
    bool exists = false;

    PG_TRY();
    {
        exists = iceberg_meta_namespace_exists(p_namespace);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        iceberg_err_rethrow_metadata(edata, "check namespace existence");
    }
    PG_END_TRY();

    /* 4. Return result */
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum(exists ? "{\"exists\": true}" : "{\"exists\": false}")));
}


/* ---- load_namespace ---- */
PG_FUNCTION_INFO_V1(iceberg_load_namespace);

Datum
iceberg_load_namespace(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace  TEXT  (required)
     *
     * Returns: JSONB (GetNamespaceResponse)
     *   {"namespace": ["<namespace>"], "properties": {<key>: <value>, ...}}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */
    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_load_namespace: expected 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* 2. Validate p_namespace */
    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("namespace must not be empty")));

    /* 3. META GetNamespace */
    MetaNamespaceInfo *ns_info = NULL;

    PG_TRY();
    {
        ns_info = iceberg_meta_get_namespace(p_namespace);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        iceberg_err_rethrow_metadata(edata, "load namespace metadata");
    }
    PG_END_TRY();

    if (ns_info == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_NOT_FOUND),
                 errmsg("The given namespace does not exist")));

    /* 4. Construct and return response */
    {
        StringInfoData buf;

        initStringInfo(&buf);
        appendStringInfo(&buf,
            "{\"namespace\":[\"%s\"],\"properties\":%s}",
            ns_info->namespace_name, ns_info->properties);
        PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
            CStringGetDatum(buf.data)));
    }
}


/* ---- list_namespaces ---- */
PG_FUNCTION_INFO_V1(iceberg_list_namespaces);

Datum
iceberg_list_namespaces(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_parent      TEXT     (optional, default NULL)
     *   2. p_page_size   INTEGER  (optional, default 1000)
     *   3. p_page_token  TEXT     (optional, default NULL)
     *
     * Returns: JSONB (ListNamespacesResponse)
     *   {"namespaces": [["ns1"], ...], "next-page-token": "<token>"}
     *   Last page: {"namespaces": [...], "next-page-token": null}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */
    /* p_parent (optional, default NULL) */
    char *p_parent = NULL;
    if (PG_NARGS() > 0 && !PG_ARGISNULL(0))
        p_parent = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_page_size (optional, default 1000) */
    int p_page_size = 1000;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
        p_page_size = PG_GETARG_INT32(1);

    /* p_page_token (optional, default NULL) */
    char *p_page_token = NULL;
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
        p_page_token = text_to_cstring(PG_GETARG_TEXT_P(2));

    /* 2. Validate p_page_size */
    if (p_page_size < 1)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("pageSize must be >= 1")));

    /* 3. Validate p_parent exists (if non-NULL and non-empty) */
    if (p_parent != NULL && strlen(p_parent) > 0)
    {
        bool parent_exists = false;

        PG_TRY();
        {
            parent_exists = iceberg_meta_namespace_exists(p_parent);
        }
        PG_CATCH();
        {
            ErrorData *edata = CopyErrorData();
            iceberg_err_rethrow_metadata(edata, "list namespaces parent check");
        }
        PG_END_TRY();

        if (!parent_exists)
            ereport(ERROR,
                    (errcode(ERRCODE_ICEBERG_NOT_FOUND),
                     errmsg("The given namespace does not exist")));
    }

    /* 4. TODO: META ListNamespaces */
    /* TODO:
     * char *result = iceberg_meta_list_namespaces(p_parent, p_page_size, p_page_token);
     * PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(result)));
     */

    /* 5. Stub: return empty list with null next-page-token.
     * TODO: Replace with META.ListNamespaces() result once META module is available. */
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"namespaces\": [], \"next-page-token\": null}")));
}
