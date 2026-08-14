#include "cm/compile.h"
#include "cm/diag.h"
#include "cm/driver.h"
#include "cm/source.h"
#include "cm/syntax/ast.h"
#include "cm/syntax/lexer.h"
#include "cm/syntax/parser.h"
#include "cm/syntax/token_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cm_print_usage(FILE *stream)
{
    fputs("usage: cmrustc [--target TRIPLE] [ACTION]\n"
          "\n"
          "actions:\n"
          "  --help                  show this help\n"
          "  --version               show compiler and producer identity\n"
          "  --print target          print the selected target descriptor\n"
          "  --probe-source FILE...  load sources and emit span diagnostics\n"
          "  --dump-tokens FILE      emit the canonical lexer token stream\n"
          "  --dump-token-tree FILE  emit canonical nested token trees\n"
          "  --dump-ast FILE         parse and emit the canonical AST\n"
          "  --emit-c FILE -o FILE   compile a supported crate to portable C\n"
          "  --emit-cmhir FILE --crate-name NAME -o FILE\n"
          "                          emit private declaration metadata\n"
          "    --extern-cmhir NAME FILE\n"
          "                          load one private metadata dependency\n"
          "  --emit-cmhir-v2 FILE --crate-name NAME -o FILE\n"
          "                          emit exact v2 declaration metadata\n"
          "    --extern-cmhir-v2 NAME FILE\n"
          "                          load exact v2 declaration metadata\n"
          "  --emit-semantic-cmhir FILE --crate-name NAME -o FILE\n"
          "                          emit exact v1.1 OPEN semantic metadata\n"
          "    --extern-semantic-cmhir NAME FILE\n"
          "                          load exact v1.1 semantic metadata\n"
          "  --edition YEAR          select 2015, 2018, 2021, or 2024\n"
          "  --run PROGRAM [ARG...]  run a child process without a shell\n",
        stream);
}

typedef struct CmTokenDump {
    const unsigned char *source;
} CmTokenDump;

static int cm_dump_token(void *user, const struct cm_token *token)
{
    CmTokenDump *dump;
    size_t index;

    dump = (CmTokenDump *)user;
    printf("%s\t%lu\t%lu\t%lu\t%lu\t%s\t%lu\t%lu\t",
        cm_token_kind_name(token->kind),
        (unsigned long)token->start,
        (unsigned long)token->length,
        (unsigned long)token->line,
        (unsigned long)token->column,
        cm_keyword_name(token->keyword),
        (unsigned long)token->flags,
        (unsigned long)token->detail);
    for (index = 0u; index < token->length; ++index) {
        printf("%02x", (unsigned int)dump->source[token->start + index]);
    }
    putchar('\n');
    return 0;
}

static int cm_dump_tokens(const char *path, enum cm_edition edition)
{
    CmSourceSet sources;
    CmSourceId id;
    CmSourceStatus status;
    const CmSourceFile *file;
    struct cm_lexer_options options;
    struct cm_lexer_result result;
    CmTokenDump dump;

    cm_source_set_init(&sources);
    status = cm_source_load_file(&sources, path, &id);
    if (status != CM_SOURCE_OK) {
        fprintf(stderr, "cmrustc: cannot load %s: %s\n", path,
            cm_source_status_name(status));
        cm_source_set_destroy(&sources);
        return 1;
    }
    file = cm_source_get(&sources, id);
    if (file == NULL) {
        fputs("cmrustc: internal source lookup failure\n", stderr);
        cm_source_set_destroy(&sources);
        return 1;
    }
    cm_lexer_options_init(&options);
    options.edition = edition;
    options.emit_whitespace = 1;
    options.emit_comments = 1;
    dump.source = file->bytes;
    result = cm_lex((const char *)file->bytes, file->length, &options,
        cm_dump_token, &dump);
    cm_source_set_destroy(&sources);
    if (result.stopped) {
        fputs("cmrustc: token dump stopped unexpectedly\n", stderr);
        return 1;
    }
    return result.error_count == 0u ? 0 : 1;
}

static int cm_dump_token_tree(const char *path, enum cm_edition edition)
{
    CmSourceSet sources;
    CmSourceId id;
    CmSourceStatus status;
    const CmSourceFile *file;
    struct cm_lexer_options options;
    struct cm_token_tree tree;
    struct cm_token_tree_result result;
    CmStrBuf output;

    cm_source_set_init(&sources);
    status = cm_source_load_file(&sources, path, &id);
    if (status != CM_SOURCE_OK) {
        fprintf(stderr, "cmrustc: cannot load %s: %s\n", path,
            cm_source_status_name(status));
        cm_source_set_destroy(&sources);
        return 1;
    }
    file = cm_source_get(&sources, id);
    if (file == NULL) {
        fputs("cmrustc: internal source lookup failure\n", stderr);
        cm_source_set_destroy(&sources);
        return 1;
    }

    cm_lexer_options_init(&options);
    options.edition = edition;
    cm_token_tree_init(&tree);
    result = cm_token_tree_build(
        &tree,
        (const char *)file->bytes,
        file->length,
        &options
    );
    cm_str_buf_init(&output);
    cm_token_tree_dump(
        &tree,
        (const char *)file->bytes,
        file->length,
        &output
    );
    if (output.len != 0) {
        (void)fwrite(output.data, 1, output.len, stdout);
    }
    cm_str_buf_destroy(&output);
    cm_token_tree_destroy(&tree);
    cm_source_set_destroy(&sources);
    return result.lexer_error_count == 0
        && result.delimiter_error_count == 0 ? 0 : 1;
}

static int cm_dump_ast(const char *path, enum cm_edition edition)
{
    CmSourceSet sources;
    CmSourceId id;
    CmSourceStatus status;
    const CmSourceFile *file;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_source_set_init(&sources);
    status = cm_source_load_file(&sources, path, &id);
    if (status != CM_SOURCE_OK) {
        fprintf(stderr, "cmrustc: cannot load %s: %s\n", path,
            cm_source_status_name(status));
        cm_source_set_destroy(&sources);
        return 1;
    }
    file = cm_source_get(&sources, id);
    if (file == NULL) {
        fputs("cmrustc: internal source lookup failure\n", stderr);
        cm_source_set_destroy(&sources);
        return 1;
    }

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, (const char *)file->bytes, file->length,
        edition);
    if (result.error_count != 0u) {
        fprintf(stderr, "%s:%lu:%lu: parse error: %s (%lu total)\n",
            path, (unsigned long)result.first_error.line,
            (unsigned long)result.first_error.column,
            result.first_error.message, (unsigned long)result.error_count);
        cm_ast_destroy(&ast);
        cm_source_set_destroy(&sources);
        return 1;
    }
    ok = cm_ast_dump(stdout, &ast);
    cm_ast_destroy(&ast);
    cm_source_set_destroy(&sources);
    return ok ? 0 : 1;
}

static int cm_parse_edition(const char *text, enum cm_edition *edition)
{
    if (strcmp(text, "2015") == 0) {
        *edition = CM_EDITION_2015;
    } else if (strcmp(text, "2018") == 0) {
        *edition = CM_EDITION_2018;
    } else if (strcmp(text, "2021") == 0) {
        *edition = CM_EDITION_2021;
    } else if (strcmp(text, "2024") == 0) {
        *edition = CM_EDITION_2024;
    } else {
        return 0;
    }
    return 1;
}

static int cm_print_target(const CmTargetDesc *target)
{
    if (target == NULL) {
        fputs("cmrustc: no default target for this build host\n", stderr);
        return 1;
    }
    printf("triple=%s\narchitecture=%s\nos=%s\nenvironment=%s\n"
           "pointer-bits=%u\nendian=%s\n",
        target->triple, target->architecture, target->operating_system,
        target->environment, target->pointer_bits,
        target->endian == CM_ENDIAN_LITTLE ? "little" : "big");
    return 0;
}

static int cm_probe_sources(int count, char **paths)
{
    CmSourceSet sources;
    int index;
    int result;

    if (count == 0) {
        fputs("cmrustc: --probe-source requires at least one file\n", stderr);
        return 2;
    }
    cm_source_set_init(&sources);
    result = 0;
    for (index = 0; index < count; ++index) {
        CmSourceId id;
        CmSourceStatus status;
        const CmSourceFile *file;
        CmSpan span;

        status = cm_source_load_file(&sources, paths[index], &id);
        if (status != CM_SOURCE_OK) {
            fprintf(stderr, "cmrustc: cannot load %s: %s\n", paths[index],
                cm_source_status_name(status));
            result = 1;
            continue;
        }
        file = cm_source_get(&sources, id);
        if (file == NULL) {
            fputs("cmrustc: internal source lookup failure\n", stderr);
            result = 1;
            continue;
        }
        span.source = id;
        span.start = file->length == 0u ? 0u : (uint32_t)(file->length - 1u);
        span.end = span.start;
        cm_diag_emit(stdout, &sources, CM_DIAG_NOTE, span, "source loaded");
    }
    cm_source_set_destroy(&sources);
    return result;
}

static int cm_emit_cmhir_cli(int argc, char **argv, int action_index,
    enum cm_edition edition, const CmTargetDesc *target,
    enum CmCompileCmhirKind output_kind)
{
    const char *input_path;
    const char *output_path;
    const char *crate_name;
    CmCompileCmhirDependency *dependencies;
    size_t dependency_count;
    int index;
    CmCompileResult result;

    if (action_index + 1 >= argc) {
        fputs("cmrustc: --emit-cmhir requires an input file\n", stderr);
        return 2;
    }
    input_path = argv[action_index + 1];
    output_path = NULL;
    crate_name = NULL;
    dependency_count = 0u;
    dependencies = (CmCompileCmhirDependency *)calloc(
        (size_t)(argc - action_index), sizeof(*dependencies));
    if (dependencies == NULL) {
        fputs("cmrustc: cannot allocate cmhir command-line state\n", stderr);
        return 1;
    }
    index = action_index + 2;
    while (index < argc) {
        if (strcmp(argv[index], "--crate-name") == 0) {
            if (crate_name != NULL || index + 1 >= argc) {
                fputs("cmrustc: --crate-name requires one unique name\n",
                    stderr);
                free(dependencies);
                return 2;
            }
            crate_name = argv[index + 1];
            index += 2;
        } else if (strcmp(argv[index], "--extern-cmhir") == 0) {
            if (index + 2 >= argc) {
                fputs("cmrustc: --extern-cmhir requires 'NAME FILE'\n",
                    stderr);
                free(dependencies);
                return 2;
            }
            dependencies[dependency_count].extern_name = argv[index + 1];
            dependencies[dependency_count].path = argv[index + 2];
            dependencies[dependency_count].kind =
                CM_COMPILE_CMHIR_DECLARATION;
            dependency_count += 1u;
            index += 3;
        } else if (strcmp(argv[index], "--extern-semantic-cmhir") == 0) {
            if (index + 2 >= argc) {
                fputs("cmrustc: --extern-semantic-cmhir requires "
                      "'NAME FILE'\n", stderr);
                free(dependencies);
                return 2;
            }
            dependencies[dependency_count].extern_name = argv[index + 1];
            dependencies[dependency_count].path = argv[index + 2];
            dependencies[dependency_count].kind =
                CM_COMPILE_CMHIR_SEMANTIC;
            dependency_count += 1u;
            index += 3;
        } else if (strcmp(argv[index], "--extern-cmhir-v2") == 0) {
            if (index + 2 >= argc) {
                fputs("cmrustc: --extern-cmhir-v2 requires 'NAME FILE'\n",
                    stderr);
                free(dependencies);
                return 2;
            }
            dependencies[dependency_count].extern_name = argv[index + 1];
            dependencies[dependency_count].path = argv[index + 2];
            dependencies[dependency_count].kind =
                CM_COMPILE_CMHIR_DECLARATION_V2;
            dependency_count += 1u;
            index += 3;
        } else if (strcmp(argv[index], "-o") == 0) {
            if (output_path != NULL || index + 1 >= argc) {
                fputs("cmrustc: -o requires one unique output file\n",
                    stderr);
                free(dependencies);
                return 2;
            }
            output_path = argv[index + 1];
            index += 2;
        } else {
            fprintf(stderr, "cmrustc: unexpected --emit-cmhir argument: %s\n",
                argv[index]);
            free(dependencies);
            return 2;
        }
    }
    if (crate_name == NULL || output_path == NULL) {
        fputs("cmrustc: --emit-cmhir requires "
              "'FILE --crate-name NAME -o FILE'\n", stderr);
        free(dependencies);
        return 2;
    }
    result = cm_compile_emit_cmhir_kind(input_path, output_path, crate_name,
        edition, target, dependencies, dependency_count, output_kind);
    free(dependencies);
    if (result.status != CM_COMPILE_OK) {
        fprintf(stderr, "cmrustc: %s: %s\n",
            cm_compile_status_name(result.status), result.message);
        return 1;
    }
    return 0;
}

int cm_driver_main(int argc, char **argv)
{
    const CmTargetDesc *target;
    enum cm_edition edition;
    int index;

    target = cm_target_default();
    edition = CM_EDITION_2021;
    index = 1;
    while (index < argc) {
        const char *argument;

        argument = argv[index];
        if (strcmp(argument, "--target") == 0) {
            if (index + 1 >= argc) {
                fputs("cmrustc: --target requires a triple\n", stderr);
                return 2;
            }
            target = cm_target_find(argv[index + 1]);
            if (target == NULL) {
                fprintf(stderr, "cmrustc: unsupported target: %s\n",
                    argv[index + 1]);
                return 2;
            }
            index += 2;
        } else if (strcmp(argument, "--edition") == 0) {
            if (index + 1 >= argc ||
                !cm_parse_edition(argv[index + 1], &edition)) {
                fputs("cmrustc: --edition requires 2015, 2018, 2021, or 2024\n",
                    stderr);
                return 2;
            }
            index += 2;
        } else if (strcmp(argument, "--help") == 0) {
            cm_print_usage(stdout);
            return 0;
        } else if (strcmp(argument, "--version") == 0) {
            puts(cm_build_identity());
            return 0;
        } else if (strcmp(argument, "--print") == 0) {
            if (index + 1 >= argc || strcmp(argv[index + 1], "target") != 0) {
                fputs("cmrustc: only '--print target' is implemented\n", stderr);
                return 2;
            }
            return cm_print_target(target);
        } else if (strcmp(argument, "--probe-source") == 0) {
            return cm_probe_sources(argc - index - 1, &argv[index + 1]);
        } else if (strcmp(argument, "--dump-tokens") == 0) {
            if (index + 1 >= argc) {
                fputs("cmrustc: --dump-tokens requires a file\n", stderr);
                return 2;
            }
            return cm_dump_tokens(argv[index + 1], edition);
        } else if (strcmp(argument, "--dump-token-tree") == 0) {
            if (index + 1 >= argc) {
                fputs("cmrustc: --dump-token-tree requires a file\n", stderr);
                return 2;
            }
            return cm_dump_token_tree(argv[index + 1], edition);
        } else if (strcmp(argument, "--dump-ast") == 0) {
            if (index + 1 >= argc) {
                fputs("cmrustc: --dump-ast requires a file\n", stderr);
                return 2;
            }
            return cm_dump_ast(argv[index + 1], edition);
        } else if (strcmp(argument, "--emit-c") == 0) {
            CmCompileResult result;

            if (index + 3 >= argc || strcmp(argv[index + 2], "-o") != 0
                || index + 4 != argc) {
                fputs("cmrustc: --emit-c requires 'FILE -o FILE'\n",
                    stderr);
                return 2;
            }
            result = cm_compile_emit_c(argv[index + 1], argv[index + 3],
                edition, target);
            if (result.status != CM_COMPILE_OK) {
                fprintf(stderr, "cmrustc: %s: %s\n",
                    cm_compile_status_name(result.status), result.message);
                return 1;
            }
            return 0;
        } else if (strcmp(argument, "--emit-cmhir") == 0) {
            return cm_emit_cmhir_cli(argc, argv, index, edition, target,
                CM_COMPILE_CMHIR_DECLARATION);
        } else if (strcmp(argument, "--emit-cmhir-v2") == 0) {
            return cm_emit_cmhir_cli(argc, argv, index, edition, target,
                CM_COMPILE_CMHIR_DECLARATION_V2);
        } else if (strcmp(argument, "--emit-semantic-cmhir") == 0) {
            return cm_emit_cmhir_cli(argc, argv, index, edition, target,
                CM_COMPILE_CMHIR_SEMANTIC);
        } else if (strcmp(argument, "--run") == 0) {
            CmProcessStatus status;

            if (index + 1 >= argc) {
                fputs("cmrustc: --run requires a program\n", stderr);
                return 2;
            }
            if (!cm_process_run(&argv[index + 1], &status)) {
                fputs("cmrustc: unable to launch child process\n", stderr);
                return 1;
            }
            if (status.exited) {
                return status.exit_code;
            }
            fprintf(stderr, "cmrustc: child terminated by signal %d\n",
                status.signal_number);
            return 1;
        } else if (argument[0] == '-') {
            fprintf(stderr, "cmrustc: unknown option: %s\n", argument);
            return 2;
        } else {
            CmSourceSet sources;
            CmSourceId id;
            CmSourceStatus status;
            CmSpan span;

            cm_source_set_init(&sources);
            status = cm_source_load_file(&sources, argument, &id);
            if (status != CM_SOURCE_OK) {
                fprintf(stderr, "cmrustc: cannot load %s: %s\n", argument,
                    cm_source_status_name(status));
                cm_source_set_destroy(&sources);
                return 1;
            }
            span.source = id;
            span.start = 0u;
            span.end = 0u;
            cm_diag_emit(stderr, &sources, CM_DIAG_FATAL, span,
                "compiler pipeline is not implemented yet");
            cm_source_set_destroy(&sources);
            return 1;
        }
    }
    cm_print_usage(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    return cm_driver_main(argc, argv);
}
