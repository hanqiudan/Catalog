-- ============================================================================
-- iceberg_catalog.is_table_existed 测试用例
-- ============================================================================

BEGIN;

-- 1. 基础调用：表存在返回 {"exists": true}
SELECT iceberg_catalog.is_table_existed('test_ns', 'test_tbl');

-- 2. 验证返回类型为 JSONB object
SELECT jsonb_typeof(iceberg_catalog.is_table_existed('test_ns', 'test_tbl')) AS result_type;

-- 3. 验证返回结构包含 exists key
SELECT iceberg_catalog.is_table_existed('test_ns', 'test_tbl') ? 'exists' AS has_exists;

-- 4. p_namespace 为空串 → 报错
SAVEPOINT sp4;
SELECT iceberg_catalog.is_table_existed('', 'tbl');
ROLLBACK TO SAVEPOINT sp4;

-- 5. p_table 为空串 → 报错
SAVEPOINT sp5;
SELECT iceberg_catalog.is_table_existed('ns', '');
ROLLBACK TO SAVEPOINT sp5;

-- 6. p_namespace 为 NULL → 报错
SAVEPOINT sp6;
SELECT iceberg_catalog.is_table_existed(NULL, 'tbl');
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_table 为 NULL → 报错
SAVEPOINT sp7;
SELECT iceberg_catalog.is_table_existed('ns', NULL);
ROLLBACK TO SAVEPOINT sp7;

ROLLBACK;
