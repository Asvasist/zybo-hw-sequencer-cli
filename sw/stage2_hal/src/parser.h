/*
 * parser.h
 *
 * Command line parser for the sequencer CLI.
 *
 * Turns one received line such as "LED_BLINK 500" into a command ID plus up
 * to PARSER_MAX_ARGS signed 32-bit integer arguments. Pure C with no target
 * or BSP dependencies, so it is unit tested on a PC (tests/host).
 *
 * Grammar, kept deliberately small:
 *
 *   line     = [blank] [keyword {blank argument}] [blank]
 *   keyword  = a name from the command table, case-insensitive
 *   argument = ["+" | "-"] digit {digit}       decimal, must fit in int32_t
 *   blank    = one or more spaces or tabs
 *
 * The parser works on (pointer, length), never writes to the line, never
 * allocates and keeps no state between calls - so no strtok() (hidden static
 * state, modifies its input) and no strtol() (needs a NUL, skips leading
 * whitespace, accepts "0x" prefixes and reports overflow through errno).
 * Any byte outside printable ASCII is rejected rather than guessed at.
 */
#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>
#include <stdint.h>

/* Most arguments any command may take. Room for the sequencer commands still to come. */
#define PARSER_MAX_ARGS     4U

/*
 * Recognised commands. The command table in parser.c is indexed by this enum,
 * and HELP lists the commands in this order.
 */
typedef enum
{
    CMD_HELP = 0,
    CMD_STATUS,
    CMD_LED_SET,
    CMD_LED_BLINK,
    CMD_READ_TEMP,
    CMD_STOP,
    CMD_COUNT,                  /* number of commands, not a command itself */
    CMD_NONE = CMD_COUNT        /* "no command": empty line or unknown keyword */
} cmd_id_t;

typedef enum
{
    PARSER_OK = 0,
    PARSER_EMPTY,               /* blank line: nothing to do, not an error */
    PARSER_ERR_NULL_ARG,        /* line or result pointer was NULL */
    PARSER_ERR_BAD_CHARACTER,   /* control character or non-ASCII byte in the line */
    PARSER_ERR_UNKNOWN_COMMAND,
    PARSER_ERR_BAD_NUMBER,      /* argument is not a decimal integer that fits int32_t */
    PARSER_ERR_TOO_FEW_ARGS,
    PARSER_ERR_TOO_MANY_ARGS
} parser_status_t;

/* One row of the command table. */
typedef struct
{
    const char *keyword;        /* upper case, as HELP shows it */
    const char *arg_usage;      /* e.g. "<duration_ms> [period_ms]"; "" if none */
    const char *summary;        /* one line for HELP */
    uint8_t     min_args;
    uint8_t     max_args;
} parser_cmd_info_t;

typedef struct
{
    cmd_id_t command;
    uint8_t  arg_count;
    int32_t  args[PARSER_MAX_ARGS];
    size_t   error_offset;      /* start of the offending token in the line ... */
    size_t   error_length;      /* ... and its length; both 0 unless an error is returned */
} parser_result_t;

/*
 * Parses `length` bytes of `line`. The line doesn't need a terminating NUL
 * and is not modified. Only the status decides what *result means:
 *
 *   PARSER_OK                    command and args[0..arg_count-1] are valid
 *   PARSER_EMPTY                 command is CMD_NONE
 *   PARSER_ERR_BAD_CHARACTER     command is CMD_NONE; error_* is the one bad byte
 *   PARSER_ERR_UNKNOWN_COMMAND   command is CMD_NONE; error_* is the keyword
 *   PARSER_ERR_BAD_NUMBER        command is valid (to show its usage); error_* is the argument
 *   PARSER_ERR_TOO_MANY_ARGS     command is valid; error_* is the first surplus argument
 *   PARSER_ERR_TOO_FEW_ARGS      command is valid; error_* is the keyword
 *   PARSER_ERR_NULL_ARG          *result is reset if result itself isn't NULL
 */
parser_status_t parser_parse(const char *line, size_t length, parser_result_t *result);

/* Command table row for `command`, or NULL if it isn't a command (CMD_NONE included). */
const parser_cmd_info_t *parser_command_info(cmd_id_t command);

/* Short human-readable description of a status. Never NULL. */
const char *parser_status_text(parser_status_t status);

#endif /* PARSER_H */
