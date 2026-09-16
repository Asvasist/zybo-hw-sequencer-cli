/*
 * test_parser.c
 *
 * Host-side unit tests for parser.c. No target, no BSP, any C11 compiler:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -I../../src test_parser.c ../../src/parser.c -o test_parser
 *   ./test_parser
 *
 * Exit code 0 means every check passed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "parser.h"

#define LINE_BUF_LEN            96U
#define ROUNDTRIP_ITERATIONS    200000U
#define FUZZ_ITERATIONS         200000U
#define FUZZ_MAX_LEN            64U

static unsigned s_checks_run;
static unsigned s_checks_failed;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        s_checks_run++;                                                         \
        if (!(cond))                                                            \
        {                                                                       \
            s_checks_failed++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                       \
    } while (0)

/* Parses a NUL-terminated test line. */
static parser_status_t parse_str(const char *line, parser_result_t *result)
{
    return parser_parse(line, strlen(line), result);
}

/* Deterministic pseudo-random numbers (xorshift32), so failures reproduce. */
static uint32_t s_rng_state = 0x2545F491U;

static uint32_t next_random(void)
{
    s_rng_state ^= s_rng_state << 13;
    s_rng_state ^= s_rng_state >> 17;
    s_rng_state ^= s_rng_state << 5;
    return s_rng_state;
}

static void test_command_table(void)
{
    unsigned idx;

    printf("command table is complete and consistent\n");

    for (idx = 0U; idx < (unsigned)CMD_COUNT; idx++)
    {
        const parser_cmd_info_t *const info = parser_command_info((cmd_id_t)idx);
        size_t                         pos;

        CHECK(info != NULL);
        if (info == NULL)
        {
            continue;
        }

        CHECK((info->keyword != NULL) && (info->keyword[0] != '\0'));
        CHECK(info->arg_usage != NULL);
        CHECK((info->summary != NULL) && (info->summary[0] != '\0'));
        CHECK(info->min_args <= info->max_args);
        CHECK(info->max_args <= PARSER_MAX_ARGS);

        /* Keywords are stored upper case: the matcher relies on it. */
        for (pos = 0U; (info->keyword != NULL) && (info->keyword[pos] != '\0'); pos++)
        {
            const char ch = info->keyword[pos];
            CHECK(((ch >= 'A') && (ch <= 'Z')) || ((ch >= '0') && (ch <= '9')) || (ch == '_'));
        }
    }

    CHECK(parser_command_info(CMD_NONE) == NULL);
    CHECK(parser_command_info((cmd_id_t)99) == NULL);
}

static void test_every_command_parses(void)
{
    unsigned idx;

    printf("every command parses with its minimum and maximum argument count\n");

    for (idx = 0U; idx < (unsigned)CMD_COUNT; idx++)
    {
        const parser_cmd_info_t *const info = parser_command_info((cmd_id_t)idx);
        char                           line[LINE_BUF_LEN];
        parser_result_t                result;
        uint8_t                        arg;
        uint8_t                        count;

        for (count = info->min_args; count <= info->max_args; count++)
        {
            (void)snprintf(line, sizeof(line), "%s", info->keyword);
            for (arg = 0U; arg < count; arg++)
            {
                size_t used = strlen(line);
                (void)snprintf(&line[used], sizeof(line) - used, " %d", (int)(arg + 1U) * 10);
            }

            CHECK(parse_str(line, &result) == PARSER_OK);
            CHECK(result.command == (cmd_id_t)idx);
            CHECK(result.arg_count == count);
            for (arg = 0U; arg < count; arg++)
            {
                CHECK(result.args[arg] == (int32_t)((arg + 1U) * 10U));
            }
            CHECK((result.error_offset == 0U) && (result.error_length == 0U));
        }
    }
}

static void test_spec_examples(void)
{
    parser_result_t result;

    printf("examples from the specification\n");

    CHECK(parse_str("LED_BLINK 500", &result) == PARSER_OK);
    CHECK(result.command == CMD_LED_BLINK);
    CHECK(result.arg_count == 1U);
    CHECK(result.args[0] == 500);

    CHECK(parse_str("LED_BLINK 10000", &result) == PARSER_OK);
    CHECK(result.args[0] == 10000);

    CHECK(parse_str("READ_TEMP", &result) == PARSER_OK);
    CHECK(result.command == CMD_READ_TEMP);
    CHECK(result.arg_count == 0U);
}

static void test_case_and_whitespace(void)
{
    parser_result_t result;

    printf("keywords are case-insensitive, blanks are flexible\n");

    CHECK(parse_str("led_blink 500", &result) == PARSER_OK);
    CHECK(result.command == CMD_LED_BLINK);
    CHECK(parse_str("Led_Blink 500", &result) == PARSER_OK);
    CHECK(parse_str("hElP", &result) == PARSER_OK);
    CHECK(result.command == CMD_HELP);

    CHECK(parse_str("  LED_SET\t2   1  ", &result) == PARSER_OK);
    CHECK(result.command == CMD_LED_SET);
    CHECK(result.arg_count == 2U);
    CHECK((result.args[0] == 2) && (result.args[1] == 1));

    CHECK(parse_str("\t\tSTOP\t", &result) == PARSER_OK);
    CHECK(result.command == CMD_STOP);
}

static void test_empty_lines(void)
{
    parser_result_t result;

    printf("empty and blank lines\n");

    CHECK(parser_parse("", 0U, &result) == PARSER_EMPTY);
    CHECK(result.command == CMD_NONE);
    CHECK(parse_str("   ", &result) == PARSER_EMPTY);
    CHECK(parse_str("\t \t", &result) == PARSER_EMPTY);
    CHECK((result.error_offset == 0U) && (result.error_length == 0U));
}

static void test_unknown_commands(void)
{
    static const char *const unknown[] =
    {
        "FOO", "LED", "LED_SE 1 1", "LED_SETX 1 1", "HELPHELP", "_HELP", "HEL", "LED-SET 1 1", "500",
    };
    parser_result_t result;
    size_t          idx;

    printf("unknown keywords, including prefixes and extensions of real ones\n");

    for (idx = 0U; idx < (sizeof(unknown) / sizeof(unknown[0])); idx++)
    {
        CHECK(parse_str(unknown[idx], &result) == PARSER_ERR_UNKNOWN_COMMAND);
        CHECK(result.command == CMD_NONE);
        CHECK(result.error_offset == 0U);
    }

    CHECK(parse_str("  FOO 1 2", &result) == PARSER_ERR_UNKNOWN_COMMAND);
    CHECK((result.error_offset == 2U) && (result.error_length == 3U));
}

static void test_argument_count(void)
{
    parser_result_t result;

    printf("argument count checks\n");

    CHECK(parse_str("LED_BLINK", &result) == PARSER_ERR_TOO_FEW_ARGS);
    CHECK(result.command == CMD_LED_BLINK);
    CHECK((result.error_offset == 0U) && (result.error_length == 9U));

    CHECK(parse_str("  LED_SET 1", &result) == PARSER_ERR_TOO_FEW_ARGS);
    CHECK(result.command == CMD_LED_SET);
    CHECK((result.error_offset == 2U) && (result.error_length == 7U));

    CHECK(parse_str("READ_TEMP 5", &result) == PARSER_ERR_TOO_MANY_ARGS);
    CHECK(result.command == CMD_READ_TEMP);
    CHECK((result.error_offset == 10U) && (result.error_length == 1U));

    CHECK(parse_str("LED_BLINK 1 2 345", &result) == PARSER_ERR_TOO_MANY_ARGS);
    CHECK((result.error_offset == 14U) && (result.error_length == 3U));

    /* Far more tokens than args[] can hold: rejected at the first surplus one. */
    CHECK(parse_str("LED_BLINK 1 2 3 4 5 6 7 8 9 10 11 12", &result) == PARSER_ERR_TOO_MANY_ARGS);
    CHECK(result.arg_count == 2U);

    /* A surplus token that isn't even a number is still "too many". */
    CHECK(parse_str("STOP now", &result) == PARSER_ERR_TOO_MANY_ARGS);
}

static void test_valid_numbers(void)
{
    static const struct
    {
        const char *text;
        int32_t     value;
    } cases[] =
    {
        { "0", 0 }, { "500", 500 }, { "+7", 7 }, { "-1", -1 }, { "007", 7 }, { "-0", 0 },
        { "2147483647", INT32_MAX }, { "+2147483647", INT32_MAX }, { "-2147483648", INT32_MIN },
        { "-2147483647", -INT32_MAX }, { "0000000000000000000042", 42 },
    };
    char            line[LINE_BUF_LEN];
    parser_result_t result;
    size_t          idx;

    printf("valid integer arguments, including the int32_t limits\n");

    for (idx = 0U; idx < (sizeof(cases) / sizeof(cases[0])); idx++)
    {
        (void)snprintf(line, sizeof(line), "LED_BLINK %s", cases[idx].text);
        CHECK(parse_str(line, &result) == PARSER_OK);
        CHECK(result.args[0] == cases[idx].value);
    }
}

static void test_invalid_numbers(void)
{
    static const char *const bad[] =
    {
        "2147483648", "-2147483649", "99999999999", "4294967296",
        "4294967306",               /* 10 after a silent 32-bit wrap */
        "12a", "a12", "-", "+", "1-2", "0x10", "--1", "+-1", "1.5", "1,5", "5ms",
    };
    char            line[LINE_BUF_LEN];
    parser_result_t result;
    size_t          idx;

    printf("malformed and out-of-range arguments\n");

    for (idx = 0U; idx < (sizeof(bad) / sizeof(bad[0])); idx++)
    {
        (void)snprintf(line, sizeof(line), "LED_BLINK %s", bad[idx]);
        CHECK(parse_str(line, &result) == PARSER_ERR_BAD_NUMBER);
        CHECK(result.command == CMD_LED_BLINK);
        CHECK(result.error_offset == 10U);
        CHECK(result.error_length == strlen(bad[idx]));
    }

    CHECK(parse_str("LED_SET 1 on", &result) == PARSER_ERR_BAD_NUMBER);
    CHECK((result.error_offset == 10U) && (result.error_length == 2U));
}

static void test_bad_characters(void)
{
    static const char with_nul[]  = { 'H', 'E', 'L', 'P', '\0' };
    static const char arrow_key[] = { 0x1B, '[', 'A' };
    parser_result_t   result;

    printf("control and non-ASCII bytes are rejected with their position\n");

    CHECK(parse_str("LED_BLINK\x01 5", &result) == PARSER_ERR_BAD_CHARACTER);
    CHECK((result.error_offset == 9U) && (result.error_length == 1U));
    CHECK(result.command == CMD_NONE);

    CHECK(parser_parse(arrow_key, sizeof(arrow_key), &result) == PARSER_ERR_BAD_CHARACTER);
    CHECK(result.error_offset == 0U);

    CHECK(parse_str("HELP\x80", &result) == PARSER_ERR_BAD_CHARACTER);
    CHECK(result.error_offset == 4U);

    CHECK(parse_str("STOP\x7F", &result) == PARSER_ERR_BAD_CHARACTER);
    CHECK(parse_str("STOP\r", &result) == PARSER_ERR_BAD_CHARACTER);

    /* An embedded NUL is a byte like any other: length decides, not the terminator. */
    CHECK(parser_parse(with_nul, sizeof(with_nul), &result) == PARSER_ERR_BAD_CHARACTER);
    CHECK(result.error_offset == 4U);
}

static void test_length_is_respected(void)
{
    static const char unterminated[] = { 'S', 'T', 'O', 'P' };
    parser_result_t   result;

    printf("only `length` bytes are read, no NUL needed\n");

    CHECK(parser_parse(unterminated, sizeof(unterminated), &result) == PARSER_OK);
    CHECK(result.command == CMD_STOP);

    CHECK(parser_parse("READ_TEMP 5", 9U, &result) == PARSER_OK);
    CHECK(result.arg_count == 0U);

    CHECK(parser_parse("LED_BLINK 500", 12U, &result) == PARSER_OK);
    CHECK(result.args[0] == 50);

    CHECK(parser_parse("HELP\0garbage\x01", 4U, &result) == PARSER_OK);
}

static void test_null_and_reset(void)
{
    parser_result_t result;

    printf("NULL arguments and result reset between calls\n");

    CHECK(parser_parse("HELP", 4U, NULL) == PARSER_ERR_NULL_ARG);

    CHECK(parse_str("LED_SET 1 2", &result) == PARSER_OK);
    CHECK(parser_parse(NULL, 4U, &result) == PARSER_ERR_NULL_ARG);
    CHECK(result.command == CMD_NONE);
    CHECK(result.arg_count == 0U);
    CHECK((result.args[0] == 0) && (result.args[1] == 0));

    CHECK(parse_str("FOO", &result) == PARSER_ERR_UNKNOWN_COMMAND);
    CHECK(parse_str("HELP", &result) == PARSER_OK);
    CHECK((result.error_offset == 0U) && (result.error_length == 0U));
}

static void test_status_text(void)
{
    int status;

    printf("every status has a description\n");

    for (status = (int)PARSER_OK; status <= (int)PARSER_ERR_TOO_MANY_ARGS; status++)
    {
        const char *const text = parser_status_text((parser_status_t)status);
        CHECK((text != NULL) && (text[0] != '\0'));
        CHECK(strcmp(text, "unknown parser status") != 0);
    }

    CHECK(strcmp(parser_status_text((parser_status_t)42), "unknown parser status") == 0);
}

static void test_random_roundtrip(void)
{
    char            line[LINE_BUF_LEN];
    parser_result_t result;
    unsigned        iter;
    unsigned        mismatches = 0U;

    printf("random int32 values survive print -> parse (%u values)\n", ROUNDTRIP_ITERATIONS);

    for (iter = 0U; iter < ROUNDTRIP_ITERATIONS; iter++)
    {
        const int32_t value = (int32_t)next_random();
        const int32_t id    = (int32_t)(next_random() % 4U);

        (void)snprintf(line, sizeof(line), "led_set  %ld\t%ld ", (long)id, (long)value);
        if ((parse_str(line, &result) != PARSER_OK) || (result.args[0] != id) || (result.args[1] != value))
        {
            mismatches++;
        }
    }

    CHECK(mismatches == 0U);
}

/*
 * Random lines glued together from keywords, numbers and junk, with random
 * blanks in between and a random cut-off. Checks the invariants that must
 * hold for any input: a known status, an error position inside the line, no
 * argument count beyond args[], and an argument count the command allows.
 */
static void test_fuzz_invariants(void)
{
    static const char *const pieces[] =
    {
        "HELP", "stop", "Status", "LED_SET", "led_blink", "READ_TEMP", "LED", "LED_BLINKS",
        "0", "-1", "+5", "500", "2147483647", "2147483648", "-2147483648", "-", "12a", "x",
        " ", "\t", "  ", "\x01", "\x7F", "\x80",
    };
    static const size_t piece_count = sizeof(pieces) / sizeof(pieces[0]);
    char                line[FUZZ_MAX_LEN];
    parser_result_t     result;
    unsigned            iter;
    unsigned            violations = 0U;
    unsigned            accepted   = 0U;

    printf("fuzzed lines keep the result invariants (%u lines)\n", FUZZ_ITERATIONS);

    for (iter = 0U; iter < FUZZ_ITERATIONS; iter++)
    {
        const unsigned  piece_total = next_random() % 7U;
        size_t          length      = 0U;
        parser_status_t status;
        unsigned        piece;

        for (piece = 0U; piece < piece_total; piece++)
        {
            const char *const text = pieces[next_random() % piece_count];
            const char *const gap  = ((next_random() % 2U) == 0U) ? " " : "\t";

            length += (size_t)snprintf(&line[length], sizeof(line) - length, "%s%s", text, gap);
            if (length >= sizeof(line))
            {
                length = sizeof(line) - 1U;
                break;
            }
        }

        /* Cut the line short now and then, mid-token included. */
        if ((length > 0U) && ((next_random() % 4U) == 0U))
        {
            length = next_random() % length;
        }

        status = parser_parse(line, length, &result);

        if (((int)status < (int)PARSER_OK) || ((int)status > (int)PARSER_ERR_TOO_MANY_ARGS) ||
            (status == PARSER_ERR_NULL_ARG) ||
            ((result.error_offset + result.error_length) > length) ||
            (result.arg_count > PARSER_MAX_ARGS))
        {
            violations++;
        }

        if (status == PARSER_OK)
        {
            const parser_cmd_info_t *const info = parser_command_info(result.command);

            accepted++;
            if ((info == NULL) || (result.arg_count < info->min_args) || (result.arg_count > info->max_args))
            {
                violations++;
            }
        }
    }

    CHECK(violations == 0U);
    CHECK(accepted > 0U);   /* the generator does produce valid lines too */
}

int main(void)
{
    test_command_table();
    test_every_command_parses();
    test_spec_examples();
    test_case_and_whitespace();
    test_empty_lines();
    test_unknown_commands();
    test_argument_count();
    test_valid_numbers();
    test_invalid_numbers();
    test_bad_characters();
    test_length_is_respected();
    test_null_and_reset();
    test_status_text();
    test_random_roundtrip();
    test_fuzz_invariants();

    printf("\n%u checks, %u failed\n", s_checks_run, s_checks_failed);
    return (s_checks_failed == 0U) ? 0 : 1;
}
