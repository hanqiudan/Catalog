-- ============================================================================
-- iceberg_catalog.add_column 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- 1. 基础调用：传入 4 个必填参数，返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.add_column(
    'test_ns',
    'test_tbl',
    'new_col',
    'string'
)) AS result_type;

-- 2. 返回结构包含两个顶层 key
SELECT
    iceberg_catalog.add_column('test_ns', 'test_tbl', 'col_a', 'int') ? 'metadata-location' AS has_metadata_location,
    iceberg_catalog.add_column('test_ns', 'test_tbl', 'col_b', 'long') ? 'metadata'          AS has_metadata;

-- 3. 传入 p_column_doc 参数
SELECT iceberg_catalog.add_column(
    'test_ns',
    'test_tbl',
    'col_with_doc',
    'decimal(10,2)',
    p_column_doc => 'A documented column'
);

-- 4. p_namespace 为空串 → 报错
SAVEPOINT sp4;
SELECT iceberg_catalog.add_column('', 'tbl', 'col', 'string');
ROLLBACK TO SAVEPOINT sp4;

-- 5. p_table 为空串 → 报错
SAVEPOINT sp5;
SELECT iceberg_catalog.add_column('ns', '', 'col', 'string');
ROLLBACK TO SAVEPOINT sp5;

-- 6. p_column_name 为空串 → 报错
SAVEPOINT sp6;
SELECT iceberg_catalog.add_column('ns', 'tbl', '', 'string');
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_column_type 为空串 → 报错
SAVEPOINT sp7;
SELECT iceberg_catalog.add_column('ns', 'tbl', 'col', '');
ROLLBACK TO SAVEPOINT sp7;

ROLLBACK;
