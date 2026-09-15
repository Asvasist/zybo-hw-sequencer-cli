/*
 * parser.c
 *
 * Command line parser. See parser.h for the grammar and the result contract.
 */
#include "parser.h"

#include <stdbool.h>

#define ASCII_TAB               0x09
#define ASCII_PRINTABLE_FIRST   0x20    /* ' ' */
#define ASCII_PRINTABLE_LAST    0x7E    /* '~' */

#define DECIMAL_BASE            10U

/* Largest magnitudes an int32_t can take, as unsigned so the checks can't overflow. */
#define INT32_POS_MAGNITUDE     ((uint32_t)INT32_MAX)
#define INT32_NEG_MAGNITUDE     ((uint32_t)INT32_MAX + 1U)

/*
 * The command table, indexed by cmd_id_t. The parser only checks the number of
 * arguments; what the values mean and which ranges are valid belongs to the
 * code that executes the command (the sequencer FSM, from stage 3 on).
 */
static const parser_cmd_info_t s_cmd_table[] =
{
    [CMD_HELP]      = { "HELP",      "",                          "list the commands",                      0U, 0U },
    [CMD_STATUS]    = { "STATUS",    "",                          "UART receive statistics",                0U, 0U },
    [CMD_LED_SET]   = { "LED_SET",   "<led_id> <0|1>",            "switch one LED off or on",               2U, 2U },
    [CMD_LED_BLINK] = { "LED_BLINK", "<duration_ms> [period_ms]", "blink an LED for a while, non-blocking", 1U, 2U },
    [CMD_READ_TEMP] = { "READ_TEMP", "",                          "read the die temperature",               0U, 0U },
    [CMD_STOP]      = { "STOP",      "",                          "abort the running sequence",             0U, 0U },
};

/* A command added to the enum without a table row (or the other way round) fails the build. */
_Static_assert((sizeof(s_cmd_table) / sizeof(s_cmd_table[0])) == (size_t)CMD_COUNT,
               "s_cmd_table must have exactly one row per cmd_id_t");

/* Walks a line token by token. A token is a run of non-blank characters. */
typedef struct
{
    const char *line;
    size_t      length;
    size_t      pos;
} token_scanner_t;

static bool is_blank(char ch)
{
    return (ch == ' ') || (ch == '\t');
}

static bool is_allowed_char(char ch)
{
    const unsigned char code = (unsigned char)ch;

    return ((code >= ASCII_PRINTABLE_FIRST) && (code <= ASCII_PRINTABLE_LAST)) || (code == ASCII_TAB);
}

static char to_upper_ascii(char ch)
{
    return ((ch >= 'a') && (ch <= 'z')) ? (char)((ch - 'a') + 'A') : ch;
}

/* Finds the next token. Returns false when only blanks (or nothing) are left. */
static bool next_token(token_scanner_t *scanner, size_t *start_out, size_t *length_out)
{
    while ((scanner->pos < scanner->length) && is_blank(scanner->line[scanner->pos]))
    {
        scanner->pos++;
    }

    if (scanner->pos == scanner->length)
    {
        return false;
    }

    *start_out = scanner->pos;
    while ((scanner->pos < scanner->length) && !is_blank(scanner->line[scanner->pos]))
    {
        scanner->pos++;
    }
    *length_out = scanner->pos - *start_out;

    return true;
}

/*
 * Case-insensitive, whole-token comparison: "LED" must not match "LED_SET",
 * and neither must "LED_SETX". Token bytes are already known to be printable,
 * so a token character can never equal the keyword's NUL - the loop stops at
 * the end of a shorter keyword without reading past it.
 */
static bool keyword_matches(const char *keyword, const char *token, size_t token_length)
{
    size_t idx;

    for (idx = 0U; idx < token_length; idx++)
    {
        if (to_upper_ascii(token[idx]) != keyword[idx])
        {
            return false;
        }
    }

    return keyword[token_length] == '\0';
}

static cmd_id_t find_command(const char *token, size_t token_length)
{
    size_t idx;

    for (idx = 0U; idx < (size_t)CMD_COUNT; idx++)
    {
        if ((s_cmd_table[idx].keyword != NULL) && keyword_matches(s_cmd_table[idx].keyword, token, token_length))
        {
            return (cmd_id_t)idx;
        }
    }

    return CMD_NONE;
}

/*
 * Strict decimal to int32_t: an optional sign followed by one or more digits,
 * nothing else. Overflow is caught before it happens, one digit at a time:
 * magnitude * 10 + digit <= limit  <=>  magnitude <= (limit - digit) / 10.
 * *value_out is only written on success.
 */
static bool parse_int32(const char *text, size_t length, int32_t *value_out)
{
    size_t   idx       = 0U;
    bool     negative  = false;
    uint32_t magnitude = 0U;
    uint32_t limit;

    if ((length > 0U) && ((text[0] == '+') || (text[0] == '-')))
    {
        negative = (text[0] == '-');
        idx      = 1U;
    }

    if (idx == length)
    {
        return false;   /* empty, or a sign with no digits */
    }

    limit = negative ? INT32_NEG_MAGNITUDE : INT32_POS_MAGNITUDE;

    for (; idx < length; idx++)
    {
        uint32_t digit;

        if ((text[idx] < '0') || (text[idx] > '9'))
        {
            return false;
        }

        digit = (uint32_t)(text[idx] - '0');
        if (magnitude > ((limit - digit) / DECIMAL_BASE))
        {
            return false;
        }
        magnitude = (magnitude * DECIMAL_BASE) + digit;
    }

    if (!negative)
    {
        *value_out = (int32_t)magnitude;
    }
    else if (magnitude == INT32_NEG_MAGNITUDE)
    {
        *value_out = INT32_MIN;     /* -(int32_t)2147483648 would overflow */
    }
    else
    {
        *value_out = -(int32_t)magnitude;
    }

    return true;
}

static void result_reset(parser_result_t *result)
{
    size_t idx;

    result->command   = CMD_NONE;
    result->arg_count = 0U;
    for (idx = 0U; idx < (size_t)PARSER_MAX_ARGS; idx++)
    {
        result->args[idx] = 0;
    }
    result->error_offset = 0U;
    result->error_length = 0U;
}

static parser_status_t result_error(parser_result_t *result, parser_status_t status, size_t offset, size_t length)
{
    result->error_offset = offset;
    result->error_length = length;
    return status;
}

parser_status_t parser_parse(const char *line, size_t length, parser_result_t *result)
{
    token_scanner_t          scanner;
    const parser_cmd_info_t *info;
    size_t                   keyword_start;
    size_t                   keyword_length;
    size_t                   token_start;
    size_t                   token_length;
    size_t                   idx;
    uint8_t                  max_args;

    if (result == NULL)
    {
        return PARSER_ERR_NULL_ARG;
    }

    result_reset(result);

    if (line == NULL)
    {
        return PARSER_ERR_NULL_ARG;
    }

    /* Check every byte up front, so the tokenizer below only ever sees clean text. */
    for (idx = 0U; idx < length; idx++)
    {
        if (!is_allowed_char(line[idx]))
        {
            return result_error(result, PARSER_ERR_BAD_CHARACTER, idx, 1U);
        }
    }

    scanner.line   = line;
    scanner.length = length;
    scanner.pos    = 0U;

    if (!next_token(&scanner, &keyword_start, &keyword_length))
    {
        return PARSER_EMPTY;
    }

    result->command = find_command(&line[keyword_start], keyword_length);
    if (result->command == CMD_NONE)
    {
        return result_error(result, PARSER_ERR_UNKNOWN_COMMAND, keyword_start, keyword_length);
    }

    info = &s_cmd_table[result->command];

    /* args[] must never overflow, even if a table row asked for more than it can hold. */
    max_args = (info->max_args < PARSER_MAX_ARGS) ? info->max_args : (uint8_t)PARSER_MAX_ARGS;

    while (next_token(&scanner, &token_start, &token_length))
    {
        if (result->arg_count >= max_args)
        {
            return result_error(result, PARSER_ERR_TOO_MANY_ARGS, token_start, token_length);
        }

        if (!parse_int32(&line[token_start], token_length, &result->args[result->arg_count]))
        {
            return result_error(result, PARSER_ERR_BAD_NUMBER, token_start, token_length);
        }

        result->arg_count++;
    }

    if (result->arg_count < info->min_args)
    {
        return result_error(result, PARSER_ERR_TOO_FEW_ARGS, keyword_start, keyword_length);
    }

    return PARSER_OK;
}

const parser_cmd_info_t *parser_command_info(cmd_id_t command)
{
    if ((unsigned int)command >= (unsigned int)CMD_COUNT)
    {
        return NULL;
    }

    return &s_cmd_table[command];
}

const char *parser_status_text(parser_status_t status)
{
    switch (status)
    {
    case PARSER_OK:                  return "ok";
    case PARSER_EMPTY:               return "empty line";
    case PARSER_ERR_NULL_ARG:        return "internal error, NULL argument";
    case PARSER_ERR_BAD_CHARACTER:   return "invalid character";
    case PARSER_ERR_UNKNOWN_COMMAND: return "unknown command";
    case PARSER_ERR_BAD_NUMBER:      return "argument is not a 32-bit decimal integer";
    case PARSER_ERR_TOO_FEW_ARGS:    return "too few arguments";
    case PARSER_ERR_TOO_MANY_ARGS:   return "too many arguments";
    }

    return "unknown parser status";
}
