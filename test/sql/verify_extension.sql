BEGIN;

INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES ('external_demo', 'demo_ns', '{"env":"test","owner":"catalog"}'::jsonb);

INSERT INTO iceberg_catalog.iceberg_tables(
    catalog_name,
    table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
)
VALUES (
    'external_demo',
    'demo_ns',
    'demo_tbl',
    'file:///tmp/v2.metadata.json',
    'file:///tmp/v1.metadata.json'
);

UPDATE iceberg_catalog.iceberg_tables
SET metadata_location = 'file:///tmp/v3.metadata.json',
    previous_metadata_location = 'file:///tmp/v2.metadata.json'
WHERE catalog_name = 'external_demo'
  AND table_namespace = 'demo_ns'
  AND table_name = 'demo_tbl';

SELECT
    catalog_name,
    table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
FROM iceberg_catalog.iceberg_tables
WHERE table_namespace = 'demo_ns';

SELECT
    namespace,
    property_key,
    property_value
FROM iceberg_catalog.iceberg_namespace_properties
WHERE catalog_name = 'external_demo'
  AND namespace = 'demo_ns'
ORDER BY property_key;

DELETE FROM iceberg_catalog.iceberg_tables
WHERE catalog_name = 'external_demo'
  AND table_namespace = 'demo_ns'
  AND table_name = 'demo_tbl';

SELECT count(*)
FROM iceberg_catalog.tables_external
WHERE catalog_name = 'external_demo'
  AND namespace = 'demo_ns'
  AND table_name = 'demo_tbl';

ROLLBACK;
