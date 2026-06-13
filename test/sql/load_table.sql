-- ============================================================================
-- iceberg_catalog.load_table 测试用例
-- ============================================================================

BEGIN;

-- 1. 基础调用：返回 JSONB object
SELECT jsonb_typeof(iceberg_catalog.load_table('test_ns', 'test_tbl')) AS result_type;

-- 2. 返回结构包含三个顶层 key
SELECT
    iceberg_catalog.load_table('test_ns', 'test_tbl') ? 'metadata-location' AS has_metadata_location,
    iceberg_catalog.load_table('test_ns', 'test_tbl') ? 'metadata'          AS has_metadata,
    iceberg_catalog.load_table('test_ns', 'test_tbl') ? 'config'            AS has_config;

-- 3. 返回完整 LoadTableResult
SELECT iceberg_catalog.load_table('prod_ns', 'big_tbl');

-- 4. p_namespace 为空串 → 报错
SAVEPOINT sp4;
SELECT iceberg_catalog.load_table('', 'tbl');
ROLLBACK TO SAVEPOINT sp4;

-- 5. p_table 为空串 → 报错
SAVEPOINT sp5;
SELECT iceberg_catalog.load_table('ns', '');
ROLLBACK TO SAVEPOINT sp5;

-- 6. p_namespace 为 NULL → 报错
SAVEPOINT sp6;
SELECT iceberg_catalog.load_table(NULL, 'tbl');
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_table 为 NULL → 报错
SAVEPOINT sp7;
SELECT iceberg_catalog.load_table('ns', NULL);
ROLLBACK TO SAVEPOINT sp7;

ROLLBACK;
