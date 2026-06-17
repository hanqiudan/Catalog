-- ============================================================================
-- iceberg_catalog.list_tables 测试用例
-- ============================================================================

BEGIN;

-- ### Setup: create namespaces and tables for metadata verification ###
SELECT iceberg_catalog.create_namespace('lt_ns', '{}'::jsonb);
SELECT iceberg_catalog.create_namespace('lt_empty_ns', '{}'::jsonb);
SELECT iceberg_catalog.create_namespace('lt_page_ns', '{}'::jsonb);

-- Create 3 tables in lt_ns
SELECT iceberg_catalog.create_table(
    'lt_ns', 'tbl_a',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true}]}'::jsonb
) AS create_a;

SELECT iceberg_catalog.create_table(
    'lt_ns', 'tbl_b',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true}]}'::jsonb
) AS create_b;

SELECT iceberg_catalog.create_table(
    'lt_ns', 'tbl_c',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true}]}'::jsonb
) AS create_c;

-- Create 5 tables in lt_page_ns for pagination tests
SELECT iceberg_catalog.create_table(
    'lt_page_ns', 'p01',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true}]}'::jsonb
);
SELECT iceberg_catalog.create_table(
    'lt_page_ns', 'p02',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true}]}'::jsonb
);
SELECT iceberg_catalog.create_table(
    'lt_page_ns', 'p03',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true}]}'::jsonb
);
SELECT iceberg_catalog.create_table(
    'lt_page_ns', 'p04',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true}]}'::jsonb
);
SELECT iceberg_catalog.create_table(
    'lt_page_ns', 'p05',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true}]}'::jsonb
);

-- ============================================================================
-- T1: 基础调用 — 返回 JSONB object，包含顶层 key
-- ============================================================================
SELECT jsonb_typeof(iceberg_catalog.list_tables('lt_ns')) AS t1_result_type;

SELECT
    iceberg_catalog.list_tables('lt_ns') ? 'identifiers'     AS t1_has_identifiers,
    iceberg_catalog.list_tables('lt_ns') ? 'next-page-token' AS t1_has_next_page_token;

-- ============================================================================
-- T2: 验证 list_tables 返回创建的表（元信息验证）
-- ============================================================================

-- 验证 identifiers 数组包含 3 个元素
SELECT jsonb_array_length(
    (iceberg_catalog.list_tables('lt_ns'))->'identifiers'
) = 3 AS t2_total_count;

-- 验证每行有 namespace 和 name 字段
SELECT
    bool_and(elem ? 'namespace') AS t2_has_namespace,
    bool_and(elem ? 'name')      AS t2_has_name
FROM jsonb_array_elements(
    (iceberg_catalog.list_tables('lt_ns'))->'identifiers'
) AS elem;

-- 验证 namespace 数组格式：["lt_ns"]
SELECT bool_and(elem->'namespace' = '["lt_ns"]'::jsonb) AS t2_namespace_format
FROM jsonb_array_elements(
    (iceberg_catalog.list_tables('lt_ns'))->'identifiers'
) AS elem;

-- ============================================================================
-- T3: 验证返回的表名与 tables_internal 一致
-- ============================================================================

SELECT
    count(*) = 3 AS t3_list_count,
    bool_or(table_name = 'tbl_a') AS t3_has_tbl_a,
    bool_or(table_name = 'tbl_b') AS t3_has_tbl_b,
    bool_or(table_name = 'tbl_c') AS t3_has_tbl_c
FROM iceberg_catalog.tables_internal
WHERE namespace = 'lt_ns';

-- ============================================================================
-- T4: 空 namespace 返回空列表
-- ============================================================================

SELECT jsonb_array_length(
    (iceberg_catalog.list_tables('lt_empty_ns'))->'identifiers'
) = 0 AS t4_empty_identifiers;

SELECT (iceberg_catalog.list_tables('lt_empty_ns'))->>'next-page-token' IS NULL
    OR (iceberg_catalog.list_tables('lt_empty_ns'))->>'next-page-token' = 'null'
    AS t4_null_next_token;

-- ============================================================================
-- T5: 分页 — page_size = 2，总共有 5 个表，第一页应有 next-page-token
-- ============================================================================

SELECT jsonb_array_length(
    (iceberg_catalog.list_tables('lt_page_ns', 2))->'identifiers'
) = 2 AS t5_page1_count;

SELECT (iceberg_catalog.list_tables('lt_page_ns', 2))->>'next-page-token' IS NOT NULL
    AND (iceberg_catalog.list_tables('lt_page_ns', 2))->>'next-page-token' <> 'null'
    AS t5_page1_has_token;

-- ============================================================================
-- T6: 分页 — 第二页使用 first-page token 继续
-- ============================================================================

CREATE TEMP TABLE _lt_page_token AS
SELECT (iceberg_catalog.list_tables('lt_page_ns', 2))->>'next-page-token' AS token;

-- Page 2: should return 2 more tables
SELECT jsonb_array_length(
    (iceberg_catalog.list_tables('lt_page_ns', 2,
        (SELECT token FROM _lt_page_token)))->'identifiers'
) = 2 AS t6_page2_count;

-- ============================================================================
-- T7: 分页 — page_size 足够大时不返回 next-page-token
-- ============================================================================

SELECT
    jsonb_array_length(
        (iceberg_catalog.list_tables('lt_page_ns', 100))->'identifiers'
    ) = 5 AS t7_all_count,
    ((iceberg_catalog.list_tables('lt_page_ns', 100))->>'next-page-token' IS NULL
     OR (iceberg_catalog.list_tables('lt_page_ns', 100))->>'next-page-token' = 'null')
    AS t7_no_next_token;

-- ============================================================================
-- T8: 参数校验
-- ============================================================================

-- p_page_size = 1（边界值）
SELECT iceberg_catalog.list_tables('lt_ns', 1);

-- p_page_size = 0 → 报错
SAVEPOINT sp_size0;
SELECT iceberg_catalog.list_tables('lt_ns', 0);
ROLLBACK TO SAVEPOINT sp_size0;

-- p_namespace 为空串 → 报错
SAVEPOINT sp_empty_ns;
SELECT iceberg_catalog.list_tables('');
ROLLBACK TO SAVEPOINT sp_empty_ns;

-- p_namespace 为 NULL → 报错
SAVEPOINT sp_null_ns;
SELECT iceberg_catalog.list_tables(NULL);
ROLLBACK TO SAVEPOINT sp_null_ns;

-- ============================================================================
-- T9: namespace 不存在 → 报错
-- ============================================================================

SAVEPOINT sp_missing_ns;
SELECT iceberg_catalog.list_tables('nonexistent_ns');
ROLLBACK TO SAVEPOINT sp_missing_ns;

-- ============================================================================
-- T10: 无效 page_token → 报错
-- ============================================================================

SAVEPOINT sp_bad_token;
SELECT iceberg_catalog.list_tables('lt_ns', 10, 'not-a-valid-base64-token!!!');
ROLLBACK TO SAVEPOINT sp_bad_token;

-- Cleanup
DROP TABLE IF EXISTS _lt_page_token;

ROLLBACK;
