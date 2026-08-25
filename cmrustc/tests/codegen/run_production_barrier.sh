#!/bin/sh
set -eu

compile_source=${1:-src/driver/compile.c}
case "$compile_source" in
    /*) ;;
    *) compile_source=$PWD/$compile_source ;;
esac

test -f "$compile_source"
function_body=$(sed -n \
    '/^CmCompileResult cm_compile_emit_c_with_dependencies(/,/^CmCompileResult cm_compile_emit_c(/p' \
    "$compile_source")
test -n "$function_body"

for required in \
    cm_compile_admit_instance_closure \
    cm_compile_publish_reachable_mir \
    cm_c_emit_admitted_program \
    cm_c_emit_executable_recipe_program
do
    printf '%s\n' "$function_body" | grep -q "$required"
done

for required in \
    cm_semantic_admit_regions_canonical_instance_closure \
    cm_mir_publication_begin_regions
do
    grep -q "$required" "$compile_source"
done

for forbidden in \
    legacy_semantic \
    'cm_mir_lower_body(' \
    'cm_c_emit_program('
do
    if grep -F -q "$forbidden" "$compile_source"; then
        printf '%s\n' "production emit-C bypass found: $forbidden" >&2
        exit 1
    fi
done

printf '%s\n' 'production emit-C semantic boundary passed'
