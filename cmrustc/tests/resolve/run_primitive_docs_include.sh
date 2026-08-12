#!/bin/sh
set -eu

if [ "${RUST190_CORE_ROOT:-}" = "" ]; then
    echo "RUST190_CORE_ROOT must name the Rust 1.90 library/core directory" >&2
    exit 2
fi

if [ ! -f "${RUST190_CORE_ROOT}/src/primitive_docs.rs" ]; then
    echo "missing ${RUST190_CORE_ROOT}/src/primitive_docs.rs" >&2
    exit 2
fi

: "${BUILD_DIR:=build}"

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "${repo_root}/cmrustc"
make BUILD_DIR="${BUILD_DIR}" "${BUILD_DIR}/test_module_graph"
RUST190_CORE_ROOT="${RUST190_CORE_ROOT}" "./${BUILD_DIR}/test_module_graph"
