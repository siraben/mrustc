CC ?= cc
CXX ?= c++
BUILD_DIR ?= build

CPPFLAGS ?=
CPPFLAGS += -Iinclude
HEADER_FILES := $(wildcard include/cm/*.h include/cm/*/*.h)
CFLAGS ?= -O2
LDFLAGS ?=
LDLIBS ?=

STRICT_CC ?= cc
STRICT_CFLAGS ?= -O2 -std=c99 -pedantic -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes
TCC ?= tcc
TCC_CFLAGS ?= -O2 -std=c99 -Wall -Werror -DCM_REQUIRE_TCC=1

PROGRAM := $(BUILD_DIR)/cmrustc
TEST_PROGRAM := $(BUILD_DIR)/test-source-diag
TARGET_CFG_TEST_PROGRAM := $(BUILD_DIR)/test-target-cfg

BASE_SOURCES := \
	src/base/alloc.c \
	src/base/arena.c \
	src/base/buf.c \
	src/base/interner.c \
	src/base/map.c \
	src/base/vec.c
DRIVER_SOURCES := \
	src/driver/cfg.c \
	src/driver/compile.c \
	src/driver/diag.c \
	src/driver/driver.c \
	src/driver/source.c
SYNTAX_SOURCES := \
	src/syntax/ast.c \
	src/syntax/lexer.c \
	src/syntax/parser.c \
	src/syntax/token.c \
	src/syntax/token_tree.c
MACRO_SOURCES := \
	src/macro/ast_builtin.c \
	src/macro/builtin.c \
	src/macro/cfg.c \
	src/macro/expand.c \
	src/macro/item_macro_plan.c \
	src/macro/rules_match.c \
	src/macro/rules_parse.c \
	src/macro/rules_reparse.c \
	src/macro/rules_transcribe.c \
	src/macro/syntax_adapter.c
RESOLVE_SOURCES := \
	src/resolve/dependency_macro.c \
	src/resolve/module_graph.c \
	src/resolve/imports.c
HIR_SOURCES := \
	src/hir/admission.c \
	src/hir/body.c \
	src/hir/body_owner.c \
	src/hir/finalization.c \
	src/hir/goal_table.c \
	src/hir/instance.c \
	src/hir/layout.c \
	src/hir/library.c \
	src/hir/metadata.c \
	src/hir/metadata_codec.c \
	src/hir/lower.c \
	src/hir/model.c \
	src/hir/module_map.c \
	src/hir/param_env.c \
	src/hir/projection.c \
	src/hir/projection_normalizer.c \
	src/hir/semantic.c \
	src/hir/semantic_body.c \
	src/hir/semantic_item.c \
	src/hir/semantic_results.c \
	src/hir/trait_solver.c \
	src/hir/typeck.c \
	src/hir/type_alias.c \
	src/hir/dump.c
MIR_SOURCES := \
	src/mir/model.c \
	src/mir/lower.c \
	src/mir/dump.c
CODEGEN_SOURCES := \
	src/codegen/c.c
LIBRARY_SOURCES := $(BASE_SOURCES) $(DRIVER_SOURCES) $(SYNTAX_SOURCES) $(MACRO_SOURCES) $(RESOLVE_SOURCES) $(HIR_SOURCES) $(MIR_SOURCES) $(CODEGEN_SOURCES)
PROGRAM_SOURCES := $(LIBRARY_SOURCES) src/driver/main.c
PROGRAM_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(PROGRAM_SOURCES))
BASE_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(BASE_SOURCES))
SYNTAX_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(SYNTAX_SOURCES))
MACRO_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(MACRO_SOURCES))
HIR_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(HIR_SOURCES))
HIR_MODEL_OBJECTS := \
	$(BUILD_DIR)/obj/hir/model.o \
	$(BUILD_DIR)/obj/hir/dump.o
HIR_PROJECTION_OBJECTS := \
	$(BUILD_DIR)/obj/hir/model.o \
	$(BUILD_DIR)/obj/hir/projection.o \
	$(BUILD_DIR)/obj/hir/type_alias.o
HIR_TYPECK_OBJECTS := \
	$(BUILD_DIR)/obj/hir/model.o \
	$(BUILD_DIR)/obj/hir/typeck.o
HIR_TRAIT_SOLVER_OBJECTS := \
	$(BUILD_DIR)/obj/hir/model.o \
	$(BUILD_DIR)/obj/hir/finalization.o \
	$(BUILD_DIR)/obj/hir/typeck.o \
	$(BUILD_DIR)/obj/hir/type_alias.o \
	$(BUILD_DIR)/obj/hir/projection.o \
	$(BUILD_DIR)/obj/hir/param_env.o \
	$(BUILD_DIR)/obj/hir/trait_solver.o
HIR_GOAL_TABLE_OBJECTS := \
	$(HIR_TRAIT_SOLVER_OBJECTS) \
	$(BUILD_DIR)/obj/hir/goal_table.o
HIR_PROJECTION_NORMALIZER_OBJECTS := \
	$(HIR_TRAIT_SOLVER_OBJECTS) \
	$(BUILD_DIR)/obj/hir/projection_normalizer.o
HIR_SEMANTIC_OBJECTS := \
	$(HIR_GOAL_TABLE_OBJECTS) \
	$(BUILD_DIR)/obj/hir/projection_normalizer.o \
	$(BUILD_DIR)/obj/hir/semantic.o
HIR_SEMANTIC_BODY_OBJECTS := \
	$(HIR_SEMANTIC_OBJECTS) \
	$(BUILD_DIR)/obj/hir/body_owner.o \
	$(BUILD_DIR)/obj/hir/semantic_body.o
HIR_SEMANTIC_ITEM_OBJECTS := \
	$(HIR_SEMANTIC_OBJECTS) \
	$(BUILD_DIR)/obj/hir/semantic_item.o
LIBRARY_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(LIBRARY_SOURCES))
BASE_TEST_SOURCES := $(sort $(wildcard tests/base/test_*.c))
BASE_TEST_PROGRAMS := $(patsubst tests/base/%.c,$(BUILD_DIR)/tests/base/%,$(BASE_TEST_SOURCES))
MACRO_TEST_SOURCES := $(sort $(wildcard tests/macro/test_*.c))
MACRO_TEST_PROGRAMS := $(patsubst tests/macro/%.c,$(BUILD_DIR)/tests/macro/%,$(MACRO_TEST_SOURCES))
LEXER_TEST_PROGRAM := $(BUILD_DIR)/test_lexer
PARSER_TEST_PROGRAM := $(BUILD_DIR)/test_parser
PARSER_FRAGMENT_TEST_PROGRAM := $(BUILD_DIR)/test_parser_fragments
TOKEN_TREE_TEST_PROGRAM := $(BUILD_DIR)/test_token_tree
MODULE_GRAPH_TEST_PROGRAM := $(BUILD_DIR)/test_module_graph
IMPORT_TEST_PROGRAM := $(BUILD_DIR)/test_imports
HIR_TEST_PROGRAM := $(BUILD_DIR)/test_hir_model
HIR_LOWER_TEST_PROGRAM := $(BUILD_DIR)/test_hir_lower
HIR_MODULE_MAP_TEST_PROGRAM := $(BUILD_DIR)/test_hir_module_map
HIR_GRAPH_LOWER_TEST_PROGRAM := $(BUILD_DIR)/test_hir_graph_lower
HIR_LIBRARY_TEST_PROGRAM := $(BUILD_DIR)/test_hir_library
HIR_METADATA_TEST_PROGRAM := $(BUILD_DIR)/test_hir_metadata
HIR_METADATA_PROCESS_SCRIPT := tests/hir/run_metadata_process.sh
HIR_DRIVER_METADATA_SCRIPT := tests/hir/run_driver_metadata.sh
HIR_METADATA_CODEC_TEST_PROGRAM := $(BUILD_DIR)/test_hir_metadata_codec
HIR_TYPE_ALIAS_TEST_PROGRAM := $(BUILD_DIR)/test_hir_type_alias
HIR_PROJECTION_TEST_PROGRAM := $(BUILD_DIR)/test_hir_projection
HIR_TYPECK_TEST_PROGRAM := $(BUILD_DIR)/test_hir_typeck
HIR_TRAIT_SOLVER_TEST_PROGRAM := $(BUILD_DIR)/test_hir_trait_solver
HIR_PARAM_ENV_TEST_PROGRAM := $(BUILD_DIR)/test_hir_param_env
HIR_GOAL_TABLE_TEST_PROGRAM := $(BUILD_DIR)/test_hir_goal_table
HIR_PROJECTION_NORMALIZER_TEST_PROGRAM := $(BUILD_DIR)/test_hir_projection_normalizer
HIR_SEMANTIC_TEST_PROGRAM := $(BUILD_DIR)/test_hir_semantic
HIR_SEMANTIC_BODY_TEST_PROGRAM := $(BUILD_DIR)/test_hir_semantic_body
HIR_SEMANTIC_ITEM_TEST_PROGRAM := $(BUILD_DIR)/test_hir_semantic_item
HIR_INSTANCE_TEST_PROGRAM := $(BUILD_DIR)/test_hir_instance
HIR_ADMISSION_TEST_PROGRAM := $(BUILD_DIR)/test_hir_admission
HIR_SEMANTIC_RESULTS_TEST_PROGRAM := $(BUILD_DIR)/test_hir_semantic_results
HIR_BODY_TEST_PROGRAM := $(BUILD_DIR)/test_hir_body
HIR_LAYOUT_TEST_PROGRAM := $(BUILD_DIR)/test_hir_layout
MIR_MODEL_TEST_PROGRAM := $(BUILD_DIR)/test_mir_model
MIR_LOWER_TEST_PROGRAM := $(BUILD_DIR)/test_mir_lower
CODEGEN_C_TEST_PROGRAM := $(BUILD_DIR)/test_codegen_c
CORE_HIR_PROBE_PROGRAM := $(BUILD_DIR)/probe_core_hir
AST_DUMP_PROGRAM := $(BUILD_DIR)/dump_ast
AST_DIFFERENTIAL_SCRIPT := tests/syntax/run_ast_differential.sh
ABI_PROBE_SCRIPT := tests/abi/run.sh

.PHONY: all test check strict tcc check-all check-ast-differential stage0-hcc clean

all: $(PROGRAM)

$(PROGRAM): $(PROGRAM_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(PROGRAM_OBJECTS) $(LDLIBS)

$(BUILD_DIR)/obj/%.o: src/%.c $(HEADER_FILES)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/obj/hir/library.o: src/hir/library_internal.h
$(BUILD_DIR)/obj/hir/metadata.o: src/hir/library_internal.h src/hir/metadata_codec.h
$(BUILD_DIR)/obj/hir/metadata_codec.o: src/hir/metadata_codec.h
$(BUILD_DIR)/obj/hir/admission.o: src/hir/semantic_results_internal.h
$(BUILD_DIR)/obj/hir/semantic_body.o: src/hir/semantic_body_internal.h
$(BUILD_DIR)/obj/hir/semantic_results.o: src/hir/semantic_results_internal.h

$(TEST_PROGRAM): tests/test_source_diag.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_source_diag.c $(LIBRARY_OBJECTS) $(LDLIBS)

$(TARGET_CFG_TEST_PROGRAM): tests/test_target_cfg.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(BUILD_DIR)/tests/base/%: tests/base/%.c $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(BASE_OBJECTS) $(LDLIBS)

$(BUILD_DIR)/tests/macro/%: tests/macro/%.c $(MACRO_OBJECTS) $(SYNTAX_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(MACRO_OBJECTS) $(SYNTAX_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(LEXER_TEST_PROGRAM): tests/syntax/test_lexer.c $(SYNTAX_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(SYNTAX_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(PARSER_TEST_PROGRAM): tests/syntax/test_parser.c $(SYNTAX_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(SYNTAX_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(PARSER_FRAGMENT_TEST_PROGRAM): tests/syntax/test_fragments.c $(SYNTAX_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(SYNTAX_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(TOKEN_TREE_TEST_PROGRAM): tests/syntax/test_token_tree.c $(SYNTAX_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(SYNTAX_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(MODULE_GRAPH_TEST_PROGRAM): tests/resolve/test_module_graph.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(IMPORT_TEST_PROGRAM): tests/resolve/test_imports.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_TEST_PROGRAM): tests/hir/test_model.c $(HIR_MODEL_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_MODEL_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(HIR_LOWER_TEST_PROGRAM): tests/hir/test_lower.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_MODULE_MAP_TEST_PROGRAM): tests/hir/test_module_map.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_GRAPH_LOWER_TEST_PROGRAM): tests/hir/test_graph_lower.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_LIBRARY_TEST_PROGRAM): tests/hir/test_library.c src/hir/library_internal.h $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_METADATA_TEST_PROGRAM): tests/hir/test_metadata.c src/hir/library_internal.h src/hir/metadata_codec.h $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_METADATA_CODEC_TEST_PROGRAM): tests/hir/test_metadata_codec.c src/hir/metadata_codec.h $(BUILD_DIR)/obj/hir/metadata_codec.o $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/hir $(CFLAGS) $(LDFLAGS) -o $@ $< $(BUILD_DIR)/obj/hir/metadata_codec.o $(BASE_OBJECTS) $(LDLIBS)

$(HIR_TYPE_ALIAS_TEST_PROGRAM): tests/hir/test_type_alias.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_PROJECTION_TEST_PROGRAM): tests/hir/test_projection.c $(HIR_PROJECTION_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_PROJECTION_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(HIR_TYPECK_TEST_PROGRAM): tests/hir/test_typeck.c $(HIR_TYPECK_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_TYPECK_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(HIR_TRAIT_SOLVER_TEST_PROGRAM): tests/hir/test_trait_solver.c $(HIR_TRAIT_SOLVER_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_TRAIT_SOLVER_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(HIR_PARAM_ENV_TEST_PROGRAM): tests/hir/test_param_env.c $(HIR_TRAIT_SOLVER_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_TRAIT_SOLVER_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(BUILD_DIR)/obj/hir/trait_solver.o: src/hir/trait_solver_internal.h
$(BUILD_DIR)/obj/hir/projection_normalizer.o: src/hir/trait_solver_internal.h

$(HIR_GOAL_TABLE_TEST_PROGRAM): tests/hir/test_goal_table.c src/hir/trait_solver_internal.h $(HIR_GOAL_TABLE_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_GOAL_TABLE_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(HIR_PROJECTION_NORMALIZER_TEST_PROGRAM): tests/hir/test_projection_normalizer.c $(HIR_PROJECTION_NORMALIZER_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_PROJECTION_NORMALIZER_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(HIR_SEMANTIC_TEST_PROGRAM): tests/hir/test_semantic.c $(HIR_SEMANTIC_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_SEMANTIC_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(HIR_SEMANTIC_BODY_TEST_PROGRAM): tests/hir/test_semantic_body.c $(HIR_SEMANTIC_BODY_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_SEMANTIC_BODY_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(HIR_SEMANTIC_ITEM_TEST_PROGRAM): tests/hir/test_semantic_item.c $(HIR_SEMANTIC_ITEM_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(HIR_SEMANTIC_ITEM_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

$(BUILD_DIR)/obj/hir/instance.o: src/hir/instance_internal.h

$(HIR_INSTANCE_TEST_PROGRAM): tests/hir/test_instance.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_ADMISSION_TEST_PROGRAM): tests/hir/test_admission.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_SEMANTIC_RESULTS_TEST_PROGRAM): tests/hir/test_semantic_results.c src/hir/semantic_results_internal.h $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_BODY_TEST_PROGRAM): tests/hir/test_body.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(HIR_LAYOUT_TEST_PROGRAM): tests/hir/test_layout.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(MIR_MODEL_TEST_PROGRAM): tests/mir/test_model.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(MIR_LOWER_TEST_PROGRAM): tests/mir/test_lower.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(CODEGEN_C_TEST_PROGRAM): tests/codegen/test_c.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(CORE_HIR_PROBE_PROGRAM): tools/probe_core_hir.c $(LIBRARY_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBRARY_OBJECTS) $(LDLIBS)

$(AST_DUMP_PROGRAM): tools/dump_ast.c $(SYNTAX_OBJECTS) $(BASE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(SYNTAX_OBJECTS) $(BASE_OBJECTS) $(LDLIBS)

test check: all $(TEST_PROGRAM) $(TARGET_CFG_TEST_PROGRAM) $(BASE_TEST_PROGRAMS) $(MACRO_TEST_PROGRAMS) $(LEXER_TEST_PROGRAM) $(PARSER_TEST_PROGRAM) $(PARSER_FRAGMENT_TEST_PROGRAM) $(TOKEN_TREE_TEST_PROGRAM) $(MODULE_GRAPH_TEST_PROGRAM) $(IMPORT_TEST_PROGRAM) $(HIR_TEST_PROGRAM) $(HIR_LOWER_TEST_PROGRAM) $(HIR_MODULE_MAP_TEST_PROGRAM) $(HIR_GRAPH_LOWER_TEST_PROGRAM) $(HIR_LIBRARY_TEST_PROGRAM) $(HIR_METADATA_TEST_PROGRAM) $(HIR_METADATA_CODEC_TEST_PROGRAM) $(HIR_TYPE_ALIAS_TEST_PROGRAM) $(HIR_PROJECTION_TEST_PROGRAM) $(HIR_TYPECK_TEST_PROGRAM) $(HIR_TRAIT_SOLVER_TEST_PROGRAM) $(HIR_PARAM_ENV_TEST_PROGRAM) $(HIR_GOAL_TABLE_TEST_PROGRAM) $(HIR_PROJECTION_NORMALIZER_TEST_PROGRAM) $(HIR_SEMANTIC_TEST_PROGRAM) $(HIR_SEMANTIC_BODY_TEST_PROGRAM) $(HIR_SEMANTIC_ITEM_TEST_PROGRAM) $(HIR_INSTANCE_TEST_PROGRAM) $(HIR_ADMISSION_TEST_PROGRAM) $(HIR_SEMANTIC_RESULTS_TEST_PROGRAM) $(HIR_BODY_TEST_PROGRAM) $(HIR_LAYOUT_TEST_PROGRAM) $(MIR_MODEL_TEST_PROGRAM) $(MIR_LOWER_TEST_PROGRAM) $(CODEGEN_C_TEST_PROGRAM) $(AST_DUMP_PROGRAM)
	@for test_program in $(BASE_TEST_PROGRAMS); do $$test_program; done
	@for test_program in $(MACRO_TEST_PROGRAMS); do $$test_program; done
	$(TARGET_CFG_TEST_PROGRAM)
	$(LEXER_TEST_PROGRAM) tests/syntax/fixtures/lexer_cases.rs
	$(PARSER_TEST_PROGRAM) tests/syntax/fixtures/parser_items.rs tests/syntax/fixtures/parser_items.ast
	$(PARSER_TEST_PROGRAM) tests/syntax/fixtures/parser_expr.rs tests/syntax/fixtures/parser_expr.ast
	$(PARSER_TEST_PROGRAM) tests/syntax/fixtures/parser_macros.rs tests/syntax/fixtures/parser_macros.ast
	$(PARSER_TEST_PROGRAM) tests/syntax/fixtures/rust190_pending.rs
	$(PARSER_FRAGMENT_TEST_PROGRAM)
	$(TOKEN_TREE_TEST_PROGRAM)
	$(MODULE_GRAPH_TEST_PROGRAM)
	$(IMPORT_TEST_PROGRAM)
	$(HIR_TEST_PROGRAM)
	$(HIR_LOWER_TEST_PROGRAM)
	$(HIR_MODULE_MAP_TEST_PROGRAM)
	$(HIR_GRAPH_LOWER_TEST_PROGRAM)
	$(HIR_LIBRARY_TEST_PROGRAM)
	$(HIR_METADATA_TEST_PROGRAM)
	$(HIR_METADATA_PROCESS_SCRIPT) $(abspath $(HIR_METADATA_TEST_PROGRAM))
	CMRUSTC=$(abspath $(PROGRAM)) FIXTURE_ROOT=$(abspath tests/hir/fixtures) \
		$(HIR_DRIVER_METADATA_SCRIPT)
	$(HIR_METADATA_CODEC_TEST_PROGRAM)
	$(HIR_TYPE_ALIAS_TEST_PROGRAM)
	$(HIR_PROJECTION_TEST_PROGRAM)
	$(HIR_TYPECK_TEST_PROGRAM)
	$(HIR_TRAIT_SOLVER_TEST_PROGRAM)
	$(HIR_PARAM_ENV_TEST_PROGRAM)
	$(HIR_GOAL_TABLE_TEST_PROGRAM)
	$(HIR_PROJECTION_NORMALIZER_TEST_PROGRAM)
	$(HIR_SEMANTIC_TEST_PROGRAM)
	$(HIR_SEMANTIC_BODY_TEST_PROGRAM)
	$(HIR_SEMANTIC_ITEM_TEST_PROGRAM)
	$(HIR_INSTANCE_TEST_PROGRAM)
	$(HIR_ADMISSION_TEST_PROGRAM)
	$(HIR_SEMANTIC_RESULTS_TEST_PROGRAM)
	$(HIR_BODY_TEST_PROGRAM)
	$(HIR_LAYOUT_TEST_PROGRAM)
	$(MIR_MODEL_TEST_PROGRAM)
	$(MIR_LOWER_TEST_PROGRAM)
	$(CODEGEN_C_TEST_PROGRAM)
	CC="$(CC)" CFLAGS="$(CFLAGS)" $(ABI_PROBE_SCRIPT)
	CMRUSTC=$(abspath $(PROGRAM)) TEST_SOURCE_DIAG=$(abspath $(TEST_PROGRAM)) \
		EXPECT_TCC=$(if $(CM_REQUIRE_TCC),1,0) tests/run.sh

strict:
	$(MAKE) BUILD_DIR=build/strict CC="$(STRICT_CC)" CFLAGS="$(STRICT_CFLAGS)" test

tcc:
	$(MAKE) BUILD_DIR=build/tcc CC="$(TCC)" CFLAGS="$(TCC_CFLAGS)" CM_REQUIRE_TCC=1 test

check-all: strict tcc

check-ast-differential:
	CC="$(CC)" CXX="$(CXX)" $(AST_DIFFERENTIAL_SCRIPT)

stage0-hcc:
	STAGE0_TCC_ROOT="$(STAGE0_TCC_ROOT)" BUILD_DIR=build/stage0-hcc \
		tests/stage0/run_hcc.sh

clean:
	rm -rf build
