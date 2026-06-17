-- ============================================================================
-- iceberg_catalog.load_namespace — metadata 层正确性验证
-- ============================================================================

BEGIN;

-- ### Setup: use create_namespace ###
SELECT iceberg_catalog.create_namespace('test_ns', '{"env":"test"}'::jsonb);
SELECT iceberg_catalog.create_namespace('sales', '{"region":"us"}'::jsonb);
SELECT iceberg_catalog.create_namespace('accounting', '{"owner":"Ralph","created_at":"1452120468"}'::jsonb);
SELECT iceberg_catalog.create_namespace('empty_props_ns', '{}'::jsonb);
SELECT iceberg_catalog.create_namespace('ns-with-dash', '{}'::jsonb);
SELECT iceberg_catalog.create_namespace('ns_with_underscore', '{}'::jsonb);
SELECT iceberg_catalog.create_namespace('NS123MixedCase', '{}'::jsonb);

-- ============================================================================
-- T1: 正常加载 — 返回类型与结构校验
-- ============================================================================

-- 返回合法 JSONB object
SELECT jsonb_typeof(iceberg_catalog.load_namespace('test_ns')) = 'object' AS t1_result_type;

-- 返回结构包含 "namespace" 和 "properties" 两个顶层 key
SELECT iceberg_catalog.load_namespace('test_ns') ? 'namespace'  AS t1_has_namespace,
       iceberg_catalog.load_namespace('test_ns') ? 'properties' AS t1_has_properties;

-- "namespace" 字段为数组，且包含传入的命名空间
SELECT jsonb_typeof(iceberg_catalog.load_namespace('sales')->'namespace') = 'array' AS t1_namespace_type,
       iceberg_catalog.load_namespace('sales')->'namespace'->>0 = 'sales'    AS t1_first_element;

-- ============================================================================
-- T2: properties 正确存储与读取
-- ============================================================================

-- 自定义 properties
SELECT iceberg_catalog.load_namespace('accounting')->'properties'->>'owner' = 'Ralph' AS t2_custom_props;

-- 空 properties
SELECT iceberg_catalog.load_namespace('empty_props_ns')->'properties' = '{}'::jsonb AS t2_empty_props;

-- 默认空 properties (create_namespace without props)
SELECT iceberg_catalog.load_namespace('ns-with-dash')->'properties' = '{}'::jsonb AS t2_default_props;

-- ============================================================================
-- T3: 验证写入 namespaces 表
-- ============================================================================

SELECT namespace IS NOT NULL AND properties IS NOT NULL AS t3_persisted
FROM iceberg_catalog.namespaces
WHERE catalog_name = current_database()::text
  AND namespace = 'test_ns';

-- ============================================================================
-- T4: 参数校验
-- ============================================================================

SAVEPOINT sp1;
SELECT iceberg_catalog.load_namespace('');
ROLLBACK TO SAVEPOINT sp1;

SAVEPOINT sp2;
SELECT iceberg_catalog.load_namespace(NULL::TEXT);
ROLLBACK TO SAVEPOINT sp2;

-- ============================================================================
-- T5: Namespace 不存在
-- ============================================================================

SAVEPOINT sp3;
SELECT iceberg_catalog.load_namespace('non_existent_namespace');
ROLLBACK TO SAVEPOINT sp3;

-- ============================================================================
-- T6: 特殊字符命名空间
-- ============================================================================

SELECT iceberg_catalog.load_namespace('ns-with-dash')->'namespace'->>0 = 'ns-with-dash' AS t6_dash;
SELECT iceberg_catalog.load_namespace('ns_with_underscore')->'namespace'->>0 = 'ns_with_underscore' AS t6_underscore;
SELECT iceberg_catalog.load_namespace('NS123MixedCase')->'namespace'->>0 = 'NS123MixedCase' AS t6_mixed;

ROLLBACK;
