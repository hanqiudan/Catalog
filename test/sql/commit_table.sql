-- ============================================================================
-- iceberg_catalog.commit_table 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- 1. 基础调用：传入 4 个必填参数，返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.commit_table(
    'test_ns',
    'test_tbl',
    '[{"type":"assert-table-uuid","uuid":"550e8400-e29b-41d4-a716-446655440000"}]'::JSONB,
    '[{"action":"add-snapshot","snapshot":{"snapshot-id":1,"timestamp-ms":1710000000000,"manifest-list":"s3://bucket/tbl/metadata/snap-1.avro"}}]'::JSONB
)) AS result_type;

-- 2. 返回结构包含两个顶层 key
SELECT
    iceberg_catalog.commit_table('test_ns', 'test_tbl', '[{"type":"assert-ref-snapshot-id","ref":"main","snapshot-id":0}]'::JSONB, '[{"action":"add-snapshot","snapshot":{"snapshot-id":2,"timestamp-ms":1710000000000}}]'::JSONB) ? 'metadata-location' AS has_metadata_location,
    iceberg_catalog.commit_table('test_ns', 'test_tbl', '[{"type":"assert-ref-snapshot-id","ref":"main","snapshot-id":0}]'::JSONB, '[{"action":"add-snapshot","snapshot":{"snapshot-id":2,"timestamp-ms":1710000000000}}]'::JSONB) ? 'metadata'          AS has_metadata;

-- 3. p_namespace 为空串 → 报错
SAVEPOINT sp3;
SELECT iceberg_catalog.commit_table('', 'tbl', '[{"type":"assert-table-uuid","uuid":"0"}]'::JSONB, '[{"action":"add-snapshot"}]'::JSONB);
ROLLBACK TO SAVEPOINT sp3;

-- 4. p_table 为空串 → 报错
SAVEPOINT sp4;
SELECT iceberg_catalog.commit_table('ns', '', '[{"type":"assert-table-uuid","uuid":"0"}]'::JSONB, '[{"action":"add-snapshot"}]'::JSONB);
ROLLBACK TO SAVEPOINT sp4;

-- 5. p_requirements 为 NULL → 报错
SAVEPOINT sp5;
SELECT iceberg_catalog.commit_table('ns', 'tbl', NULL::JSONB, '[{"action":"add-snapshot"}]'::JSONB);
ROLLBACK TO SAVEPOINT sp5;

-- 6. p_updates 为 NULL → 报错
SAVEPOINT sp6;
SELECT iceberg_catalog.commit_table('ns', 'tbl', '[{"type":"assert-table-uuid","uuid":"0"}]'::JSONB, NULL::JSONB);
ROLLBACK TO SAVEPOINT sp6;

ROLLBACK;
