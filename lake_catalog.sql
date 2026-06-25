CREATE SCHEMA IF NOT EXISTS lake_catalog;

CREATE TABLE IF NOT EXISTS lake_catalog."namespace" (
    namespace  TEXT PRIMARY KEY,
    properties JSONB NOT NULL DEFAULT '{}'::JSONB,
    CHECK (namespace <> ''),
    CHECK (jsonb_typeof(properties) = 'object')
);

CREATE TABLE IF NOT EXISTS lake_catalog.table_info (
    id                  BIGSERIAL PRIMARY KEY,
    namespace           TEXT NOT NULL,
    table_name          TEXT NOT NULL,
    table_uuid          TEXT NOT NULL,
    location            TEXT NOT NULL,
    metadata_location   TEXT NOT NULL,
    current_schema_id   INTEGER,
    current_snapshot_id BIGINT,
    last_updated_ms     BIGINT,
    UNIQUE (namespace, table_name),
    UNIQUE (table_uuid),
    FOREIGN KEY (namespace)
        REFERENCES lake_catalog."namespace"(namespace)
        ON DELETE RESTRICT,
    CHECK (namespace <> ''),
    CHECK (table_name <> ''),
    CHECK (table_uuid <> ''),
    CHECK (location <> ''),
    CHECK (metadata_location <> '')
);

CREATE TABLE IF NOT EXISTS lake_catalog.attribute (
    table_name        TEXT NOT NULL,
    table_uuid        TEXT NOT NULL,
    schema_id         INTEGER NOT NULL,
    attribute_id      INTEGER NOT NULL,
    attribute_name    TEXT NOT NULL,
    attribute_require BOOLEAN NOT NULL,
    attribute_type    TEXT NOT NULL,
    attribute_doc     TEXT,
    PRIMARY KEY (table_uuid, schema_id, attribute_id),
    FOREIGN KEY (table_uuid)
        REFERENCES lake_catalog.table_info(table_uuid)
        ON DELETE CASCADE,
    CHECK (table_name <> ''),
    CHECK (attribute_name <> ''),
    CHECK (attribute_type <> '')
);

CREATE TABLE IF NOT EXISTS lake_catalog.snapshot (
    table_name    TEXT NOT NULL,
    table_uuid    TEXT NOT NULL,
    snapshot_id   BIGINT NOT NULL,
    timestamp_ms  BIGINT NOT NULL,
    schema_id     INTEGER,
    PRIMARY KEY (table_uuid, snapshot_id),
    FOREIGN KEY (table_uuid)
        REFERENCES lake_catalog.table_info(table_uuid)
        ON DELETE CASCADE,
    CHECK (table_name <> '')
);

CREATE TABLE IF NOT EXISTS lake_catalog."partition" (
    table_name TEXT NOT NULL,
    table_uuid TEXT NOT NULL,
    spec_id    INTEGER NOT NULL,
    field_id   INTEGER NOT NULL,
    source_id  INTEGER NOT NULL,
    field_name TEXT NOT NULL,
    transform  TEXT NOT NULL,
    PRIMARY KEY (table_uuid, spec_id, field_id),
    FOREIGN KEY (table_uuid)
        REFERENCES lake_catalog.table_info(table_uuid)
        ON DELETE CASCADE,
    CHECK (table_name <> ''),
    CHECK (field_name <> ''),
    CHECK (transform <> '')
);

DROP FUNCTION IF EXISTS lake_catalog.clean_table_meta(TEXT, TEXT);
DROP FUNCTION IF EXISTS lake_catalog.get_lake_table_info(TEXT, TEXT);
DROP FUNCTION IF EXISTS lake_catalog.lake_table_exist(TEXT, TEXT);
DROP FUNCTION IF EXISTS lake_catalog.create_namespace(TEXT, JSONB);
DROP FUNCTION IF EXISTS lake_catalog.create_namespace(TEXT);
DROP FUNCTION IF EXISTS lake_catalog.drop_namespace(TEXT);
DROP FUNCTION IF EXISTS lake_catalog.namespace_exist(TEXT);
DROP FUNCTION IF EXISTS lake_catalog.namespace_is_empty(TEXT);
DROP FUNCTION IF EXISTS lake_catalog.get_namespace_info(TEXT);

CREATE OR REPLACE FUNCTION lake_catalog.create_namespace(
    p_namespace  TEXT,
    p_properties JSONB DEFAULT '{}'::JSONB
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    IF p_properties IS NULL THEN
        p_properties := '{}'::JSONB;
    END IF;

    BEGIN
        INSERT INTO lake_catalog."namespace"(namespace, properties)
        VALUES (p_namespace, p_properties);
    EXCEPTION WHEN unique_violation THEN
        UPDATE lake_catalog."namespace"
           SET properties = p_properties
         WHERE namespace = p_namespace;
    END;
END;
$$;

CREATE OR REPLACE FUNCTION lake_catalog.drop_namespace(
    p_namespace TEXT
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    IF EXISTS (
        SELECT 1
          FROM lake_catalog.table_info
         WHERE namespace = p_namespace
    ) THEN
        RAISE EXCEPTION 'namespace is not empty';
    END IF;

    DELETE FROM lake_catalog."namespace"
     WHERE namespace = p_namespace;
END;
$$;

CREATE OR REPLACE FUNCTION lake_catalog.namespace_exist(
    p_namespace TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN EXISTS (
        SELECT 1
          FROM lake_catalog."namespace"
         WHERE namespace = p_namespace
    );
END;
$$;

CREATE OR REPLACE FUNCTION lake_catalog.namespace_is_empty(
    p_namespace TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN NOT EXISTS (
        SELECT 1
          FROM lake_catalog.table_info
         WHERE namespace = p_namespace
    );
END;
$$;

CREATE OR REPLACE FUNCTION lake_catalog.get_namespace_info(
    p_namespace TEXT
) RETURNS JSONB
LANGUAGE plpgsql
AS $$
DECLARE
    v_properties JSONB;
BEGIN
    SELECT MAX(properties::TEXT)::JSONB
      INTO v_properties
      FROM lake_catalog."namespace"
     WHERE namespace = p_namespace;

    RETURN v_properties;
END;
$$;

CREATE OR REPLACE FUNCTION lake_catalog.get_lake_table_info(
    p_namespace  TEXT,
    p_table_name TEXT
) RETURNS TEXT
LANGUAGE plpgsql
AS $$
DECLARE
    v_metadata_location TEXT;
BEGIN
    SELECT MAX(metadata_location)
      INTO v_metadata_location
      FROM lake_catalog.table_info
     WHERE namespace = p_namespace
       AND table_name = p_table_name;

    RETURN v_metadata_location;
END;
$$;

CREATE OR REPLACE FUNCTION lake_catalog.lake_table_exist(
    p_namespace  TEXT,
    p_table_name TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN EXISTS (
        SELECT 1
          FROM lake_catalog.table_info
         WHERE namespace = p_namespace
           AND table_name = p_table_name
    );
END;
$$;

CREATE OR REPLACE FUNCTION lake_catalog.clean_table_meta(
    p_namespace  TEXT,
    p_table_name TEXT
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    DELETE FROM lake_catalog.table_info
     WHERE namespace = p_namespace
       AND table_name = p_table_name;
END;
$$;

DROP FUNCTION IF EXISTS lake_catalog.register_lake_table(TEXT, TEXT, TEXT, TEXT);

CREATE OR REPLACE FUNCTION lake_catalog.register_lake_table(
    p_namespace         TEXT,
    p_table_name        TEXT,
    p_metadata          TEXT,
    p_metadata_location TEXT
) RETURNS VOID
LANGUAGE plpgsql
AS $$
DECLARE
    v_metadata            JSONB;
    v_table_uuid          TEXT;
    v_location            TEXT;
    v_current_schema_id   INTEGER;
    v_current_snapshot_id BIGINT;
    v_last_updated_ms     BIGINT;
    v_schema              JSONB;
    v_field               JSONB;
    v_snapshot            JSONB;
    v_spec                JSONB;
    v_part_field          JSONB;
    v_schema_id           INTEGER;
    v_spec_id             INTEGER;
BEGIN
    v_metadata := p_metadata::JSONB;
    v_table_uuid := v_metadata ->> 'table-uuid';
    v_location := v_metadata ->> 'location';
    v_current_schema_id := NULLIF(v_metadata ->> 'current-schema-id', '')::INTEGER;
    v_current_snapshot_id := NULLIF(v_metadata ->> 'current-snapshot-id', '')::BIGINT;
    v_last_updated_ms := NULLIF(v_metadata ->> 'last-updated-ms', '')::BIGINT;

    IF NOT lake_catalog.namespace_exist(p_namespace) THEN
        RAISE EXCEPTION 'namespace does not exist';
    END IF;

    PERFORM lake_catalog.clean_table_meta(p_namespace, p_table_name);

    INSERT INTO lake_catalog.table_info(
        namespace,
        table_name,
        table_uuid,
        location,
        metadata_location,
        current_schema_id,
        current_snapshot_id,
        last_updated_ms
    ) VALUES (
        p_namespace,
        p_table_name,
        v_table_uuid,
        v_location,
        p_metadata_location,
        v_current_schema_id,
        v_current_snapshot_id,
        v_last_updated_ms
    );

    FOR v_schema IN
        SELECT value FROM jsonb_array_elements(COALESCE(v_metadata -> 'schemas', '[]'::JSONB))
    LOOP
        v_schema_id := NULLIF(v_schema ->> 'schema-id', '')::INTEGER;

        FOR v_field IN
            SELECT value FROM jsonb_array_elements(COALESCE(v_schema -> 'fields', '[]'::JSONB))
        LOOP
            INSERT INTO lake_catalog.attribute(
                table_name,
                table_uuid,
                schema_id,
                attribute_id,
                attribute_name,
                attribute_require,
                attribute_type,
                attribute_doc
            ) VALUES (
                p_table_name,
                v_table_uuid,
                v_schema_id,
                (v_field ->> 'id')::INTEGER,
                v_field ->> 'name',
                COALESCE((v_field ->> 'required')::BOOLEAN, FALSE),
                CASE
                    WHEN jsonb_typeof(v_field -> 'type') = 'string'
                        THEN v_field ->> 'type'
                    ELSE (v_field -> 'type')::TEXT
                END,
                v_field ->> 'doc'
            );
        END LOOP;
    END LOOP;

    FOR v_snapshot IN
        SELECT value FROM jsonb_array_elements(COALESCE(v_metadata -> 'snapshots', '[]'::JSONB))
    LOOP
        INSERT INTO lake_catalog.snapshot(
            table_name,
            table_uuid,
            snapshot_id,
            timestamp_ms,
            schema_id
        ) VALUES (
            p_table_name,
            v_table_uuid,
            (v_snapshot ->> 'snapshot-id')::BIGINT,
            (v_snapshot ->> 'timestamp-ms')::BIGINT,
            NULLIF(v_snapshot ->> 'schema-id', '')::INTEGER
        );
    END LOOP;

    FOR v_spec IN
        SELECT value FROM jsonb_array_elements(COALESCE(v_metadata -> 'partition-specs', '[]'::JSONB))
    LOOP
        v_spec_id := NULLIF(v_spec ->> 'spec-id', '')::INTEGER;

        FOR v_part_field IN
            SELECT value FROM jsonb_array_elements(COALESCE(v_spec -> 'fields', '[]'::JSONB))
        LOOP
            INSERT INTO lake_catalog."partition"(
                table_name,
                table_uuid,
                spec_id,
                field_id,
                source_id,
                field_name,
                transform
            ) VALUES (
                p_table_name,
                v_table_uuid,
                v_spec_id,
                (v_part_field ->> 'field-id')::INTEGER,
                (v_part_field ->> 'source-id')::INTEGER,
                v_part_field ->> 'name',
                CASE
                    WHEN jsonb_typeof(v_part_field -> 'transform') = 'string'
                        THEN v_part_field ->> 'transform'
                    ELSE (v_part_field -> 'transform')::TEXT
                END
            );
        END LOOP;
    END LOOP;

END;
$$;

COMMENT ON SCHEMA lake_catalog IS
'Pure SQL mock Iceberg metadata catalog for openGauss.';

COMMENT ON FUNCTION lake_catalog.register_lake_table(TEXT, TEXT, TEXT, TEXT) IS
'Registers one Iceberg table by parsing metadata.json text and expanding table, schema, snapshot, and partition metadata.';

COMMENT ON FUNCTION lake_catalog.clean_table_meta(TEXT, TEXT) IS
'Deletes one table metadata record and cascades its attributes, snapshots, and partitions.';

COMMENT ON FUNCTION lake_catalog.get_lake_table_info(TEXT, TEXT) IS
'Returns the current metadata_location for a lake table, or NULL when the table does not exist.';

COMMENT ON FUNCTION lake_catalog.lake_table_exist(TEXT, TEXT) IS
'Returns whether a lake table exists in the namespace.';

COMMENT ON FUNCTION lake_catalog.create_namespace(TEXT, JSONB) IS
'Creates or replaces namespace properties.';

COMMENT ON FUNCTION lake_catalog.drop_namespace(TEXT) IS
'Drops an empty namespace; raises an error when tables still exist in the namespace.';

COMMENT ON FUNCTION lake_catalog.namespace_exist(TEXT) IS
'Returns whether a namespace exists.';

COMMENT ON FUNCTION lake_catalog.namespace_is_empty(TEXT) IS
'Returns whether a namespace has no registered lake tables.';

COMMENT ON FUNCTION lake_catalog.get_namespace_info(TEXT) IS
'Returns namespace properties as JSONB, or NULL when absent.';
