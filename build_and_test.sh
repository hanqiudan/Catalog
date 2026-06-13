#!/bin/bash
set -e

# ============================================================
# build_and_test.sh — 编译 Catalog、安装到 openGauss、运行测试
# 要求：Docker 已安装，openGauss 源码在 /home/czm/code/openGauss-server
# ============================================================

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
OG_SRC="/home/czm/code/openGauss-server"
CONTAINER_NAME="og"
OG_PASSWORD="Test@12345"

echo "=== Step 1: Fix SQL view syntax for openGauss 5.0 ==="
SQL_FILE="${PROJECT_DIR}/iceberg_catalog--1.0.0.sql"
if grep -q 'jsonb_each_text(n.properties)' "$SQL_FILE"; then
    sed -i '120,128c\
CREATE OR REPLACE VIEW iceberg_catalog.iceberg_namespace_properties AS\
SELECT\
    catalog_name,\
    namespace,\
    (jsonb_each_text(properties)).key AS property_key,\
    (jsonb_each_text(properties)).value AS property_value\
FROM iceberg_catalog.namespaces;' "$SQL_FILE"
    echo "  View syntax fixed."
else
    echo "  View syntax already fixed, skip."
fi

echo ""
echo "=== Step 2: Start openGauss container ==="
docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
docker run --rm -d --name "$CONTAINER_NAME" \
    -e GS_PASSWORD="$OG_PASSWORD" \
    -p 15432:5432 \
    opengauss/opengauss:5.0.0
echo "  Container started, waiting for openGauss to be ready..."
sleep 5

echo ""
echo "=== Step 3: Install build tools (no openGauss lib in LD path) ==="
docker exec "$CONTAINER_NAME" bash -c '
set -e
dnf install -y gcc-c++ make || { sleep 3; dnf install -y gcc-c++ make; }
which g++ && g++ --version | head -1
'

echo ""
echo "=== Step 4: Copy source and missing headers into container ==="
docker cp "$PROJECT_DIR" "$CONTAINER_NAME":/tmp/Catalog
docker cp "${OG_SRC}/src/include/storage" "$CONTAINER_NAME":/tmp/storage_inc
echo "  Done."

echo ""
echo "=== Step 5: Build and install ==="
docker exec "$CONTAINER_NAME" bash -c '
set -e
export GAUSSHOME=/usr/local/opengauss
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
export PATH=$GAUSSHOME/bin:$PATH

echo "  Copying missing headers..."
cp -r /tmp/storage_inc/file $GAUSSHOME/include/postgresql/server/storage/

echo "  Compiling..."
cd /tmp/Catalog
make clean 2>/dev/null
PG_CONFIG=$GAUSSHOME/bin/pg_config make
echo "  Compile OK."

echo "  Installing extension..."
PG_CONFIG=$GAUSSHOME/bin/pg_config make install

mkdir -p $GAUSSHOME/lib/postgresql/proc_srclib
cp $GAUSSHOME/lib/postgresql/iceberg_catalog.so $GAUSSHOME/lib/postgresql/proc_srclib/
echo "  Install OK."
'

echo ""
echo "=== Step 6: Create extension ==="
docker exec "$CONTAINER_NAME" bash -c '
export GAUSSHOME=/usr/local/opengauss
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
export PATH=$GAUSSHOME/bin:$PATH
gsql -U gaussdb -W "$1" -d postgres -p 5432 -c "CREATE EXTENSION iceberg_catalog;"
' -- "$OG_PASSWORD"

echo ""
echo "=== Step 7: Run tests ==="
echo "--- create_table.sql ---"
docker exec "$CONTAINER_NAME" bash -c '
export GAUSSHOME=/usr/local/opengauss
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
export PATH=$GAUSSHOME/bin:$PATH
gsql -U gaussdb -W "$1" -d postgres -p 5432 -f /tmp/Catalog/test/sql/create_table.sql
' -- "$OG_PASSWORD"

echo ""
echo "--- commit_table.sql ---"
docker exec "$CONTAINER_NAME" bash -c '
export GAUSSHOME=/usr/local/opengauss
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
export PATH=$GAUSSHOME/bin:$PATH
gsql -U gaussdb -W "$1" -d postgres -p 5432 -f /tmp/Catalog/test/sql/commit_table.sql
' -- "$OG_PASSWORD"

echo ""
echo "--- add_column.sql ---"
docker exec "$CONTAINER_NAME" bash -c '
export GAUSSHOME=/usr/local/opengauss
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
export PATH=$GAUSSHOME/bin:$PATH
gsql -U gaussdb -W "$1" -d postgres -p 5432 -f /tmp/Catalog/test/sql/add_column.sql
' -- "$OG_PASSWORD"

echo ""
echo "--- verify_extension.sql ---"
docker exec "$CONTAINER_NAME" bash -c '
export GAUSSHOME=/usr/local/opengauss
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
export PATH=$GAUSSHOME/bin:$PATH
gsql -U gaussdb -W "$1" -d postgres -p 5432 -f /tmp/Catalog/test/sql/verify_extension.sql
' -- "$OG_PASSWORD"

echo ""
echo "========================================="
echo "  All done! Container \"$CONTAINER_NAME\" is still running."
echo "  Connect manually:"
echo "    docker exec -it $CONTAINER_NAME bash"
echo "    export GAUSSHOME=/usr/local/opengauss"
echo "    export LD_LIBRARY_PATH=\$GAUSSHOME/lib:\$LD_LIBRARY_PATH"
echo "    export PATH=\$GAUSSHOME/bin:\$PATH"
echo "    gsql -U gaussdb -W $OG_PASSWORD -d postgres -p 5432"
echo "========================================="
