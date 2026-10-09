#include "lexer.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <unordered_map>

namespace hoshi {

const char *tokName(Tok t) {
  switch (t) {
  case Tok::Eof: return "end of file";
  case Tok::Ident: return "identifier";
  case Tok::IntLit: return "integer literal";
  case Tok::FloatLit: return "float literal";
  case Tok::StringLit: return "string literal";
  case Tok::CharLit: return "character literal";
  case Tok::KwFn: return "'fn'";
  case Tok::KwLet: return "'let'";
  case Tok::KwVar: return "'var'";
  case Tok::KwIf: return "'if'";
  case Tok::KwElse: return "'else'";
  case Tok::KwWhile: return "'while'";
  case Tok::KwFor: return "'for'";
  case Tok::KwIn: return "'in'";
  case Tok::KwReturn: return "'return'";
  case Tok::KwBreak: return "'break'";
  case Tok::KwContinue: return "'continue'";
  case Tok::KwDefer: return "'defer'";
  case Tok::KwStruct: return "'struct'";
  case Tok::KwExtern: return "'extern'";
  case Tok::KwAs: return "'as'";
  case Tok::KwTrue: return "'true'";
  case Tok::KwFalse: return "'false'";
  case Tok::KwNull: return "'null'";
  case Tok::KwSizeof: return "'sizeof'";
  case Tok::LParen: return "'('";
  case Tok::RParen: return "')'";
  case Tok::LBrace: return "'{'";
  case Tok::RBrace: return "'}'";
  case Tok::LBracket: return "'['";
  case Tok::RBracket: return "']'";
  case Tok::Comma: return "','";
  case Tok::Semi: return "';'";
  case Tok::Colon: return "':'";
  case Tok::Dot: return "'.'";
  case Tok::DotDot: return "'..'";
  case Tok::Ellipsis: return "'...'";
  case Tok::Arrow: return "'->'";
  case Tok::Plus: return "'+'";
  case Tok::Minus: return "'-'";
  case Tok::Star: return "'*'";
  case Tok::Slash: return "'/'";
  case Tok::Percent: return "'%'";
  case Tok::Amp: return "'&'";
  case Tok::Pipe: return "'|'";
  case Tok::Caret: return "'^'";
  case Tok::Tilde: return "'~'";
  case Tok::Bang: return "'!'";
  case Tok::Shl: return "'<<'";
  case Tok::Shr: return "'>>'";
  case Tok::AmpAmp: return "'&&'";
  case Tok::PipePipe: return "'||'";
  case Tok::EqEq: return "'=='";
  case Tok::NotEq: return "'!='";
  case Tok::Lt: return "'<'";
  case Tok::LtEq: return "'<='";
  case Tok::Gt: return "'>'";
  case Tok::GtEq: return "'>='";
  case Tok::Eq: return "'='";
  case Tok::PlusEq: return "'+='";
  case Tok::MinusEq: return "'-='";
  case Tok::StarEq: return "'*='";
  case Tok::SlashEq: return "'/='";
  case Tok::PercentEq: return "'%='";
  case Tok::AmpEq: return "'&='";
  case Tok::PipeEq: return "'|='";
  case Tok::CaretEq: return "'^='";
  case Tok::ShlEq: return "'<<='";
  case Tok::ShrEq: return "'>>='";
  }
  return "?";
}

namespace {

const std::unordered_map<std::string, Tok> keywords = {
    {"fn", Tok::KwFn},         {"let", Tok::KwLet},
    {"var", Tok::KwVar},       {"if", Tok::KwIf},
    {"else", Tok::KwElse},     {"while", Tok::KwWhile},
    {"for", Tok::KwFor},       {"in", Tok::KwIn},
    {"return", Tok::KwReturn}, {"break", Tok::KwBreak},
    {"continue", Tok::KwContinue}, {"defer", Tok::KwDefer},
    {"struct", Tok::KwStruct}, {"extern", Tok::KwExtern},
    {"as", Tok::KwAs},         {"true", Tok::KwTrue},
    {"false", Tok::KwFalse},   {"null", Tok::KwNull},
    {"sizeof", Tok::KwSizeof},
};

class Lexer {
public:
  explicit Lexer(const std::string &src) : src(src) {}

  std::vector<Token> run() {
    std::vector<Token> out;
    for (;;) {
      skipTrivia();
      Token t;
      t.loc = here();
      if (pos >= src.size()) {
        t.kind = Tok::Eof;
        out.push_back(t);
        return out;
      }
      lexToken(t);
      out.push_back(std::move(t));
    }
  }

private:
  const std::string &src;
  size_t pos = 0;
  unsigned line = 1, col = 1;

  SourceLoc here() const { return {line, col}; }
  char peek(size_t off = 0) const {
    return pos + off < src.size() ? src[pos + off] : '\0';
  }
  char advance() {
    char c = src[pos++];
    if (c == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
    return c;
  }
  bool match(char c) {
    if (peek() != c)
      return false;
    advance();
    return true;
  }

  void skipTrivia() {
    for (;;) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        advance();
      } else if (c == '/' && peek(1) == '/') {
        while (pos < src.size() && peek() != '\n')
          advance();
      } else if (c == '/' && peek(1) == '*') {
        SourceLoc start = here();
        advance();
        advance();
        int depth = 1; // block comments nest
        while (depth > 0) {
          if (pos >= src.size())
            error(start, "unterminated block comment");
          if (peek() == '/' && peek(1) == '*') {
            advance(); advance(); ++depth;
          } else if (peek() == '*' && peek(1) == '/') {
            advance(); advance(); --depth;
          } else {
            advance();
          }
        }
      } else {
        return;
      }
    }
  }

  void lexToken(Token &t) {
    char c = peek();
    if (std::isalpha((unsigned char)c) || c == '_') {
      std::string word;
      while (std::isalnum((unsigned char)peek()) || peek() == '_')
        word += advance();
      auto it = keywords.find(word);
      t.kind = it != keywords.end() ? it->second : Tok::Ident;
      t.text = std::move(word);
      return;
    }
    if (std::isdigit((unsigned char)c))
      return lexNumber(t);
    if (c == '"')
      return lexString(t);
    if (c == '\'')
      return lexChar(t);

    advance();
    switch (c) {
    case '(': t.kind = Tok::LParen; return;
    case ')': t.kind = Tok::RParen; return;
    case '{': t.kind = Tok::LBrace; return;
    case '}': t.kind = Tok::RBrace; return;
    case '[': t.kind = Tok::LBracket; return;
    case ']': t.kind = Tok::RBracket; return;
    case ',': t.kind = Tok::Comma; return;
    case ';': t.kind = Tok::Semi; return;
    case ':': t.kind = Tok::Colon; return;
    case '~': t.kind = Tok::Tilde; return;
    case '.':
      if (match('.'))
        t.kind = match('.') ? Tok::Ellipsis : Tok::DotDot;
      else
        t.kind = Tok::Dot;
      return;
    case '+': t.kind = match('=') ? Tok::PlusEq : Tok::Plus; return;
    case '-':
      if (match('>'))
        t.kind = Tok::Arrow;
      else
        t.kind = match('=') ? Tok::MinusEq : Tok::Minus;
      return;
    case '*': t.kind = match('=') ? Tok::StarEq : Tok::Star; return;
    case '/': t.kind = match('=') ? Tok::SlashEq : Tok::Slash; return;
    case '%': t.kind = match('=') ? Tok::PercentEq : Tok::Percent; return;
    case '^': t.kind = match('=') ? Tok::CaretEq : Tok::Caret; return;
    case '!': t.kind = match('=') ? Tok::NotEq : Tok::Bang; return;
    case '=': t.kind = match('=') ? Tok::EqEq : Tok::Eq; return;
    case '&':
      if (match('&'))
        t.kind = Tok::AmpAmp;
      else
        t.kind = match('=') ? Tok::AmpEq : Tok::Amp;
      return;
    case '|':
      if (match('|'))
        t.kind = Tok::PipePipe;
      else
        t.kind = match('=') ? Tok::PipeEq : Tok::Pipe;
      return;
    case '<':
      if (match('<'))
        t.kind = match('=') ? Tok::ShlEq : Tok::Shl;
      else
        t.kind = match('=') ? Tok::LtEq : Tok::Lt;
      return;
    case '>':
      if (match('>'))
        t.kind = match('=') ? Tok::ShrEq : Tok::Shr;
      else
        t.kind = match('=') ? Tok::GtEq : Tok::Gt;
      return;
    }
    error(t.loc, std::string("unexpected character '") + c + "'");
  }

  void lexNumber(Token &t) {
    std::string digits;
    int base = 10;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'b' || peek(1) == 'o')) {
      advance();
      char b = advance();
      base = b == 'x' ? 16 : b == 'b' ? 2 : 8;
    }
    auto isDigit = [&](char ch) {
      if (base == 16)
        return std::isxdigit((unsigned char)ch) != 0;
      if (base == 2)
        return ch == '0' || ch == '1';
      if (base == 8)
        return ch >= '0' && ch <= '7';
      return std::isdigit((unsigned char)ch) != 0;
    };
    while (isDigit(peek()) || peek() == '_') {
      char ch = advance();
      if (ch != '_')
        digits += ch;
    }

    bool isFloat = false;
    // `1..5` is a range, not a float, so require a digit after the dot.
    if (base == 10 && peek() == '.' && std::isdigit((unsigned char)peek(1))) {
      isFloat = true;
      digits += advance();
      while (std::isdigit((unsigned char)peek()) || peek() == '_') {
        char ch = advance();
        if (ch != '_')
          digits += ch;
      }
    }
    if (base == 10 && (peek() == 'e' || peek() == 'E')) {
      isFloat = true;
      digits += advance();
      if (peek() == '+' || peek() == '-')
        digits += advance();
      if (!std::isdigit((unsigned char)peek()))
        error(here(), "expected exponent digits");
      while (std::isdigit((unsigned char)peek()))
        digits += advance();
    }
    if (std::isalpha((unsigned char)peek()) || peek() == '_')
      error(here(), "invalid character in number literal");
    if (digits.empty())
      error(t.loc, "number literal has no digits");

    errno = 0;
    if (isFloat) {
      t.kind = Tok::FloatLit;
      t.floatVal = std::strtod(digits.c_str(), nullptr);
    } else {
      t.kind = Tok::IntLit;
      t.intVal = std::strtoull(digits.c_str(), nullptr, base);
      if (errno == ERANGE)
        error(t.loc, "integer literal is too large");
    }
  }

  char lexEscape() {
    SourceLoc loc = here();
    advance(); // backslash
    char c = pos < src.size() ? advance() : '\0';
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return '\0';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case 'x': {
      int v = 0;
      for (int i = 0; i < 2; ++i) {
        char h = peek();
        if (!std::isxdigit((unsigned char)h))
          error(here(), "expected two hex digits after \\x");
        advance();
        v = v * 16 + (std::isdigit((unsigned char)h) ? h - '0'
                                                     : std::tolower(h) - 'a' + 10);
      }
      return (char)v;
    }
    }
    error(loc, "unknown escape sequence");
  }

  void lexString(Token &t) {
    advance();
    t.kind = Tok::StringLit;
    for (;;) {
      if (pos >= src.size() || peek() == '\n')
        error(t.loc, "unterminated string literal");
      if (peek() == '"') {
        advance();
        return;
      }
      t.text += peek() == '\\' ? lexEscape() : advance();
    }
  }

  void lexChar(Token &t) {
    advance();
    t.kind = Tok::CharLit;
    if (peek() == '\'' || peek() == '\n' || pos >= src.size())
      error(t.loc, "empty character literal");
    char c = peek() == '\\' ? lexEscape() : advance();
    if (!match('\''))
      error(t.loc, "unterminated character literal");
    t.intVal = (unsigned char)c;
  }
};

} // namespace

std::vector<Token> lex(const SourceFile &file) { return Lexer(file.text).run(); }

} // namespace hoshi
