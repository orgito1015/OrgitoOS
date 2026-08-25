#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum {
    TOK_EOF = 0,
    TOK_NUMBER,
    TOK_IDENT,
    TOK_STRING,           /* "..." literal; payload in Token.str_text */
    /* keywords */
    TOK_KW_INT,
    TOK_KW_BOOL,
    TOK_KW_TRUE,
    TOK_KW_FALSE,
    TOK_KW_PRINT,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_FOR,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    TOK_KW_RETURN,
    /* arithmetic */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    /* comparison */
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_EQ,
    TOK_NE,
    /* logical */
    TOK_AND,   /* && */
    TOK_OR,    /* || */
    TOK_NOT,   /* !  */
    /* assignment family */
    TOK_ASSIGN,           /* =  */
    TOK_PLUS_ASSIGN,      /* += */
    TOK_MINUS_ASSIGN,     /* -= */
    TOK_STAR_ASSIGN,      /* *= */
    TOK_SLASH_ASSIGN,     /* /= */
    TOK_PERCENT_ASSIGN,   /* %= */
    TOK_INCR,             /* ++ */
    TOK_DECR,             /* -- */
    /* punctuation */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_SEMI
} TokenKind;

#define LEX_MAX_IDENT  64
/* Maximum bytes of a string literal after escape processing (incl. NUL). */
#define LEX_MAX_STRING 256

typedef struct {
    TokenKind kind;
    int       int_value;               /* valid if kind == TOK_NUMBER */
    char      text[LEX_MAX_IDENT];     /* valid if kind == TOK_IDENT  */
    char      str_text[LEX_MAX_STRING];/* valid if kind == TOK_STRING */
    size_t    line;
    size_t    col;
} Token;

typedef struct {
    const char *input;
    size_t      pos;   /* index of next char to read from input */
    char        current;
    size_t      line;  /* 1-based line of `current` */
    size_t      col;   /* 1-based column of `current` */
    Token       current_token;
    Token       peeked_token;   /* valid iff has_peeked */
    int         has_peeked;
} Lexer;

void lexer_init(Lexer *lex, const char *input);
void lexer_next(Lexer *lex);
/* Return (without consuming) the token following current_token. */
const Token *lexer_peek(Lexer *lex);

#endif /* LEXER_H */
