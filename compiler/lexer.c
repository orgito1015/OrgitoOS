/* OrgitoCompiler lexer - ported from OrionCC. See oc_platform.h for the
 * freestanding-kernel substitutions applied throughout (ctype, exit->
 * OC_FAIL, etc). Tokenizing logic is otherwise unchanged from upstream. */
#include <stdint.h>
#include "oc_platform.h"
#include "lexer.h"

static void lexer_advance(Lexer *lex) {
    if (lex->current == '\n') {
        lex->line++;
        lex->col = 1;
    } else if (lex->current != '\0') {
        lex->col++;
    }

    if (lex->input[lex->pos] == '\0') {
        lex->current = '\0';
    } else {
        lex->current = lex->input[lex->pos++];
    }
}

static void lexer_skip_whitespace(Lexer *lex) {
    while (oc_isspace((unsigned char)lex->current)) {
        lexer_advance(lex);
    }
}

static void lexer_error(const Lexer *lex, char c) {
    OC_FAIL("Lexer error: unexpected character '%c' at line %zu, column %zu (position %zu)\n",
            c, lex->line, lex->col, lex->pos);
}

/* Scan a string literal. `line`/`col` locate the opening quote (used for
 * unterminated-string diagnostics). Recognized escapes: \n \t \\ \".
 * A raw newline or EOF inside the literal is an error. */
static void scan_string(Lexer *lex, Token *out, size_t line, size_t col) {
    char buf[LEX_MAX_STRING];
    size_t n = 0;

    lexer_advance(lex); /* consume opening '"' */
    while (lex->current != '"') {
        if (lex->current == '\0' || lex->current == '\n') {
            OC_FAIL("Lexer error: unterminated string literal starting at line %zu, column %zu\n",
                    line, col);
        }
        char c = lex->current;
        if (c == '\\') {
            lexer_advance(lex);
            switch (lex->current) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case '\\': c = '\\'; break;
                case '"':  c = '"';  break;
                default:
                    OC_FAIL("Lexer error: unknown escape sequence '\\%c' in string literal at line %zu, column %zu\n",
                            lex->current, lex->line, lex->col);
            }
        }
        if (n >= LEX_MAX_STRING - 1) {
            OC_FAIL("Lexer error: string literal too long (max %d bytes) at line %zu, column %zu\n",
                    LEX_MAX_STRING - 1, line, col);
        }
        buf[n++] = c;
        lexer_advance(lex);
    }
    lexer_advance(lex); /* consume closing '"' */

    out->kind = TOK_STRING;
    memcpy(out->str_text, buf, n);
    out->str_text[n] = '\0';
}

/* Scan one token from the current position into *out. */
static void scan_token(Lexer *lex, Token *out) {
    lexer_skip_whitespace(lex);

    size_t line = lex->line;
    size_t col = lex->col;
    out->line = line;
    out->col = col;

    if (lex->current == '\0') {
        out->kind = TOK_EOF;
        return;
    }

    if (oc_isdigit((unsigned char)lex->current)) {
        int64_t value = 0;
        const int64_t LIMIT = (int64_t)OC_INT_MAX + 1; /* 2147483648 */
        while (oc_isdigit((unsigned char)lex->current)) {
            int digit = lex->current - '0';
            value = value * 10 + digit;
            if (value > LIMIT) {
                OC_FAIL("Lexer error: number too large (overflow) at line %zu, column %zu\n",
                        line, col);
            }
            lexer_advance(lex);
        }
        out->kind = TOK_NUMBER;
        if (value <= OC_INT_MAX) {
            out->int_value = (int)value;
        } else {
            /* value == INT_MAX+1: sentinel, only valid as INT_MIN under
             * a unary minus. The parser enforces this. */
            out->int_value = OC_INT_MIN;
        }
        return;
    }

    if (oc_isalpha((unsigned char)lex->current) || lex->current == '_') {
        size_t n = 0;
        char buf[LEX_MAX_IDENT];
        while (oc_isalnum((unsigned char)lex->current) || lex->current == '_') {
            if (n < LEX_MAX_IDENT - 1) {
                buf[n++] = lex->current;
            }
            lexer_advance(lex);
        }
        buf[n] = '\0';

        TokenKind kind = TOK_IDENT;
        if (strcmp(buf, "int") == 0)           kind = TOK_KW_INT;
        else if (strcmp(buf, "bool") == 0)     kind = TOK_KW_BOOL;
        else if (strcmp(buf, "true") == 0)     kind = TOK_KW_TRUE;
        else if (strcmp(buf, "false") == 0)    kind = TOK_KW_FALSE;
        else if (strcmp(buf, "print") == 0)    kind = TOK_KW_PRINT;
        else if (strcmp(buf, "if") == 0)       kind = TOK_KW_IF;
        else if (strcmp(buf, "else") == 0)     kind = TOK_KW_ELSE;
        else if (strcmp(buf, "while") == 0)    kind = TOK_KW_WHILE;
        else if (strcmp(buf, "for") == 0)      kind = TOK_KW_FOR;
        else if (strcmp(buf, "break") == 0)    kind = TOK_KW_BREAK;
        else if (strcmp(buf, "continue") == 0) kind = TOK_KW_CONTINUE;
        else if (strcmp(buf, "return") == 0)   kind = TOK_KW_RETURN;

        out->kind = kind;
        if (kind == TOK_IDENT) {
            memcpy(out->text, buf, n + 1);
        }
        return;
    }

    char c = lex->current;
    switch (c) {
        case '(': out->kind = TOK_LPAREN;   lexer_advance(lex); return;
        case ')': out->kind = TOK_RPAREN;   lexer_advance(lex); return;
        case '{': out->kind = TOK_LBRACE;   lexer_advance(lex); return;
        case '}': out->kind = TOK_RBRACE;   lexer_advance(lex); return;
        case '[': out->kind = TOK_LBRACKET; lexer_advance(lex); return;
        case ']': out->kind = TOK_RBRACKET; lexer_advance(lex); return;
        case ',': out->kind = TOK_COMMA;    lexer_advance(lex); return;
        case ';': out->kind = TOK_SEMI;     lexer_advance(lex); return;
        case '"': scan_string(lex, out, line, col);            return;

        case '+':
            lexer_advance(lex);
            if (lex->current == '=')      { out->kind = TOK_PLUS_ASSIGN; lexer_advance(lex); }
            else if (lex->current == '+') { out->kind = TOK_INCR;        lexer_advance(lex); }
            else                          { out->kind = TOK_PLUS; }
            return;
        case '-':
            lexer_advance(lex);
            if (lex->current == '=')      { out->kind = TOK_MINUS_ASSIGN; lexer_advance(lex); }
            else if (lex->current == '-') { out->kind = TOK_DECR;         lexer_advance(lex); }
            else                          { out->kind = TOK_MINUS; }
            return;
        case '*':
            lexer_advance(lex);
            if (lex->current == '=') { out->kind = TOK_STAR_ASSIGN; lexer_advance(lex); }
            else                     { out->kind = TOK_STAR; }
            return;
        case '/':
            lexer_advance(lex);
            if (lex->current == '=') { out->kind = TOK_SLASH_ASSIGN; lexer_advance(lex); }
            else                     { out->kind = TOK_SLASH; }
            return;
        case '%':
            lexer_advance(lex);
            if (lex->current == '=') { out->kind = TOK_PERCENT_ASSIGN; lexer_advance(lex); }
            else                     { out->kind = TOK_PERCENT; }
            return;

        case '<':
            lexer_advance(lex);
            if (lex->current == '=') { out->kind = TOK_LE; lexer_advance(lex); }
            else { out->kind = TOK_LT; }
            return;
        case '>':
            lexer_advance(lex);
            if (lex->current == '=') { out->kind = TOK_GE; lexer_advance(lex); }
            else { out->kind = TOK_GT; }
            return;
        case '=':
            lexer_advance(lex);
            if (lex->current == '=') { out->kind = TOK_EQ; lexer_advance(lex); }
            else { out->kind = TOK_ASSIGN; }
            return;
        case '!':
            lexer_advance(lex);
            if (lex->current == '=') { out->kind = TOK_NE; lexer_advance(lex); }
            else { out->kind = TOK_NOT; }
            return;
        case '&':
            lexer_advance(lex);
            if (lex->current == '&') { out->kind = TOK_AND; lexer_advance(lex); }
            else { lexer_error(lex, '&'); }
            return;
        case '|':
            lexer_advance(lex);
            if (lex->current == '|') { out->kind = TOK_OR; lexer_advance(lex); }
            else { lexer_error(lex, '|'); }
            return;

        default:
            lexer_error(lex, c);
            return;
    }
}

void lexer_init(Lexer *lex, const char *input) {
    lex->input = input;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    /* Start from a defined '\0' so the first lexer_advance neither bumps the
     * column nor (if the garbage byte happened to be '\n') the line; the
     * first real character therefore sits at line 1, column 1. */
    lex->current = '\0';
    lex->has_peeked = 0;
    lexer_advance(lex);
    lexer_next(lex);
}

void lexer_next(Lexer *lex) {
    if (lex->has_peeked) {
        lex->current_token = lex->peeked_token;
        lex->has_peeked = 0;
        return;
    }
    scan_token(lex, &lex->current_token);
}

const Token *lexer_peek(Lexer *lex) {
    if (!lex->has_peeked) {
        scan_token(lex, &lex->peeked_token);
        lex->has_peeked = 1;
    }
    return &lex->peeked_token;
}
