BEGIN;

CREATE OR REPLACE FUNCTION lake_catalog.expect_error_message(
    p_sql TEXT,
    p_expected_message TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
BEGIN
    EXECUTE p_sql;
    RETURN FALSE;
EXCEPTION WHEN others THEN
    RETURN SQLERRM = p_expected_message;
END;
$$;

SELECT lake_catalog.create_namespace('sales', '{"owner":"sales-team","region":"cn"}'::JSONB);

SELECT 'namespace_created' AS check_name,
       lake_catalog.namespace_exist('sales')
       AND lake_catalog.namespace_is_empty('sales')
       AND lake_catalog.get_namespace_info('sales') = '{"owner":"sales-team","region":"cn"}'::JSONB AS passed
UNION ALL
SELECT 'missing_namespace_defaults',
       NOT lake_catalog.namespace_exist('missing')
       AND lake_catalog.namespace_is_empty('missing')
       AND lake_catalog.get_namespace_info('missing') IS NULL
ORDER BY check_name;

SELECT lake_catalog.register_lake_table(
    'sales',
    'orders',
    '{
        "table-uuid": "11111111-2222-3333-4444-555555555555",
        "location": "s3://warehouse/sales/orders",
        "current-schema-id": 1,
        "current-snapshot-id": 9002,
        "last-updated-ms": 1712345678901,
        "schemas": [
            {
                "schema-id": 1,
                "fields": [
                    {"id": 1, "name": "order_id", "required": true, "type": "long"},
                    {"id": 2, "name": "amount", "required": false, "type": {"type": "decimal", "precision": 12, "scale": 2}}
                ]
            }
        ],
        "snapshots": [
            {"snapshot-id": 9002, "timestamp-ms": 1712345678901, "schema-id": 1}
        ],
        "partition-specs": [
            {
                "spec-id": 1,
                "fields": [
                    {"field-id": 1001, "source-id": 1, "name": "order_id_bucket", "transform": "bucket[16]"}
                ]
            }
        ]
    }',
    's3://warehouse/sales/orders/metadata/00002.metadata.json'
);

SELECT 'table_registered' AS check_name,
       lake_catalog.lake_table_exist('sales', 'orders')
       AND NOT lake_catalog.namespace_is_empty('sales')
       AND lake_catalog.get_lake_table_info('sales', 'orders') = 's3://warehouse/sales/orders/metadata/00002.metadata.json'
       AND (SELECT count(*) = 1 FROM lake_catalog.table_info WHERE namespace = 'sales' AND table_name = 'orders')
       AND (SELECT count(*) = 2 FROM lake_catalog.attribute WHERE table_uuid = '11111111-2222-3333-4444-555555555555')
       AND (SELECT count(*) = 1 FROM lake_catalog.snapshot WHERE table_uuid = '11111111-2222-3333-4444-555555555555')
       AND (SELECT count(*) = 1 FROM lake_catalog."partition" WHERE table_uuid = '11111111-2222-3333-4444-555555555555') AS passed
UNION ALL
SELECT 'drop_non_empty_namespace_rejected',
       lake_catalog.expect_error_message(
           $sql$SELECT lake_catalog.drop_namespace('sales')$sql$,
           'namespace is not empty'
       )
UNION ALL
SELECT 'register_missing_namespace_rejected',
       lake_catalog.expect_error_message(
           $sql$SELECT lake_catalog.register_lake_table('missing_ns', 't', '{"table-uuid":"c","location":"s3://t"}', 's3://metadata.json')$sql$,
           'namespace does not exist'
       )
ORDER BY check_name;

SELECT lake_catalog.clean_table_meta('sales', 'orders');
SELECT lake_catalog.drop_namespace('sales');

SELECT 'namespace_dropped' AS check_name,
       NOT lake_catalog.namespace_exist('sales')
       AND lake_catalog.namespace_is_empty('sales')
       AND lake_catalog.get_namespace_info('sales') IS NULL
       AND NOT lake_catalog.lake_table_exist('sales', 'orders')
       AND lake_catalog.get_lake_table_info('sales', 'orders') IS NULL AS passed;

ROLLBACK;
