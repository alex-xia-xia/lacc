#if _XOPEN_SOURCE < 500
#  undef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 500 /* snprintf */
#endif
#include "cli.h"
#include "type.h"

#include <stdio.h>
#include <stdarg.h>

unsigned errors = 0;
int verbose_level = 0;

#define PRINT_BUF_SIZE 2048

/* Static limits on output length, for simplicity. Store arguments and sub-
 * strings from format here temporarily.
 */
static char message[PRINT_BUF_SIZE];

/* Custom implementation of printf, handling a restricted set of formatters:
 *
 *  %s,
 *  %d,
 *  %lu, %ld
 *
 * In addition, the following custom formatters representing compiler-internal
 * data structures.
 *
 *  %t  : struct typetree *
 *
 */
static int vfprintf_cc(FILE *stream, const char *format, va_list ap)
{
    int n = 0;
    const char *cursor = format;

    if (!format)
        return n;

    while (*format) {
        if (*format++ != '%' || *format == '%')
            continue;

        /* Print format string up to this point. */
        snprintf(message, format - cursor, "%s", cursor);
        n += fputs(message, stream);

        /* Print format. */
        switch (*format++) {
        case 's':
            n += fputs(va_arg(ap, char *), stream);
            break;
        case 'd':
            n += fprintf(stream, "%d", va_arg(ap, int));
            break;
        case 'l':
            switch (*format++) {
            case 'u':
                n += fprintf(stream, "%lu", va_arg(ap, unsigned long));
                break;
            case 'd':
                n += fprintf(stream, "%ld", va_arg(ap, long));
                break;
            default:
                format -= 2;
            }
            break;
        case 't':
            snprinttype(va_arg(ap, struct typetree *), message, PRINT_BUF_SIZE);
            n += fputs(message, stream);
            break;
        default:
            format--;
            break;
        }

        /* Start of un-printed format string. */
        cursor = format;
    }

    /* Print rest of format string, which contained no formatters. */
    n += fputs(cursor, stream);
    return n;
}

void verbose(const char *format, ...)
{
    if (verbose_level) {
        va_list args;
        va_start(args, format);
        vfprintf_cc(stdout, format, args);
        fputc('\n', stdout);
        va_end(args);
    }
}

void error(const char *format, ...)
{
    va_list args;

    errors++;
    va_start(args, format);
    fprintf(stderr, "(%s, %d) error: ", current_file.path, current_file.line);
    vfprintf_cc(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}