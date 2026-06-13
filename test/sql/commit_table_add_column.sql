-- ============================================================================
-- commit_table / add_column — metadata 层正确性验证
--
-- 验证 metadata 模块的真实行为：
--   1. commit_table: metadata_location 轮转 + snapshot 缓存写入
--   2. add_column:   schema 字段展开写入 + current_schema_id/last_column_id 更新
--   3. previous_metadata_location 自动滚动
--   4. 级联删除 (ON DELETE CASCADE)
-- ============================================================================

BEGIN;

-- ### Setup ###
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database()::text, 'mtt', '{}'::jsonb);

CREATE TABLE mtt_rel(id int);

INSERT INTO iceberg_catalog.tables_internal(
    relid, namespace, table_name, table_uuid,
    metadata_location, previous_metadata_location, table_location,
    last_column_id, current_schema_id, current_snapshot_id, default_spec_id
) VALUES (
    'mtt_rel'::regclass, 'mtt', 't1',
    'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0',
    's3://b/m1.json', NULL, 's3://b/t1',
    2, 0, NULL, 0
);

INSERT INTO iceberg_catalog.table_schemas(
    table_uuid, schema_id, field_position, field_id, field_name, field_required, field_type
) VALUES
    ('a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0', 0, 0, 1, 'id',   true,  'long'),
    ('a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0', 0, 1, 2, 'data', false, 'string');

INSERT INTO iceberg_catalog.partition_specs(table_uuid, spec_id, field_position)
VALUES ('a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0', 0, -1);

-- ============================================================================
-- T1: commit_table → metadata_location 轮转 + snapshot 写入
-- ============================================================================

-- 初始: metadata_location = 's3://b/m1.json', previous NULL, snapshots 空
SELECT metadata_location AS t1_meta_before,
       previous_metadata_location IS NULL AS t1_prev_null
FROM iceberg_catalog.tables_internal
WHERE namespace = 'mtt' AND table_name = 't1';

SELECT count(*) = 0 AS t1_snap_empty
FROM iceberg_catalog.snapshots
WHERE table_uuid = 'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0';

SELECT iceberg_commit_table('mtt', 't1',
    '[]'::jsonb,
    '[{"action":"add-snapshot","snapshot":{"snapshot-id":100,"timestamp-ms":999000,"manifest-list":"s3://m","summary":{"operation":"append"},"schema-id":0}}]'::jsonb
);

-- 验证: metadata_location 已更新, previous 自动轮转
SELECT metadata_location AS t1_meta_after,
       previous_metadata_location AS t1_prev_after
FROM iceberg_catalog.tables_internal
WHERE namespace = 'mtt' AND table_name = 't1';

-- 验证: snapshot 已写入 (snapshot_id=100, timestamp_ms=999000)
SELECT snapshot_id, timestamp_ms AS t1_snap_row
FROM iceberg_catalog.snapshots
WHERE table_uuid = 'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0';

-- ============================================================================
-- T2: add_column → schema 展开写入 + current_schema_id/last_column_id 更新
-- ============================================================================

-- 初始: 2 fields, last_column_id=2, current_schema_id=0
SELECT count(*) AS t2_schema_before
FROM iceberg_catalog.table_schemas
WHERE table_uuid = 'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0';

SELECT last_column_id, current_schema_id AS t2_state_before
FROM iceberg_catalog.tables_internal
WHERE namespace = 'mtt' AND table_name = 't1';

SELECT iceberg_add_column('mtt', 't1', 'col3', 'string', 'third column');

-- 验证: schema_id=1 的新字段已写入
SELECT field_id, field_name, schema_id AS t2_new_field
FROM iceberg_catalog.table_schemas
WHERE table_uuid = 'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0'
ORDER BY schema_id, field_position;

-- 验证: current_schema_id→1, last_column_id→10
SELECT last_column_id, current_schema_id AS t2_state_after
FROM iceberg_catalog.tables_internal
WHERE namespace = 'mtt' AND table_name = 't1';

-- ============================================================================
-- T3: previous_metadata_location 经 commit+add 两次后正确
-- ============================================================================

SELECT previous_metadata_location AS t3_prev_final
FROM iceberg_catalog.tables_internal
WHERE namespace = 'mtt' AND table_name = 't1';

-- ============================================================================
-- T4: 级联删除
-- ============================================================================

DELETE FROM iceberg_catalog.tables_internal
WHERE table_uuid = 'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0';

SELECT count(*) AS t4_snaps
FROM iceberg_catalog.snapshots
WHERE table_uuid = 'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0';

SELECT count(*) AS t4_schemas
FROM iceberg_catalog.table_schemas
WHERE table_uuid = 'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0';

SELECT count(*) AS t4_specs
FROM iceberg_catalog.partition_specs
WHERE table_uuid = 'a0a0a0a0-a0a0-a0a0-a0a0-a0a0a0a0a0a0';

ROLLBACK;
