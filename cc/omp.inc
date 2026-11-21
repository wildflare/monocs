// OpenMP（-fopenmp。parse.c の終わりで取り込む）
//
// #pragma omp parallel の文は、新しい関数（__omp_fn_N(void **ctx)）として、今の関数の中にいるまま読む
// （omp_outline）。中から外の関数の局所変数を名前で見つけたら、ctx で渡されたポインタを通して使う形にする
// （omp_ref。OpenMP の既定どおり共有）。外の関数の側には「変数のアドレスを ctx に並べて
// __monocs_omp_parallel(関数, ctx, スレッドの数, if) を呼ぶ」コードを置く。ランタイムは os/libc/omp.c。
// #pragma omp for は、繰り返しの頭（for (int i = a; i < b; i += s) の形）を読み、自分の受け持ちの範囲だけを
// 回す形の字句に書き換えて読む。中の文は元の字句のまま。
// ほかに critical・atomic・barrier・single・master・masked・flush・task（その場で動かす）・taskwait、
// 節は num_threads・if・private・firstprivate・shared・default・reduction・schedule（static・dynamic・
// guided・auto・runtime）・nowait・collapse（外側の繰り返しだけを分ける）。simd・declare・ordered は読み捨てる。

TLS static int omp_counter;  // 生成する名前の番号
TLS static File *omp_file;   // 生成した字句のファイル（pragma の字句のもの）

typedef struct {
  char *p;
  size_t len, cap;
} OmpStr;

static void omp_addn(OmpStr *s, const char *t, size_t n) {
  if (s->len + n + 1 > s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 256;
    while (cap < s->len + n + 1)
      cap *= 2;
    s->p = realloc(s->p, cap);
    s->cap = cap;
  }
  memcpy(s->p + s->len, t, n);
  s->len += n;
  s->p[s->len] = '\0';
}

static void omp_add(OmpStr *s, const char *t) {
  omp_addn(s, t, strlen(t));
}

static void omp_addf(OmpStr *s, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *buf = malloc(n + 1);
  va_start(ap, fmt);
  vsnprintf(buf, n + 1, fmt, ap);
  va_end(ap);
  omp_addn(s, buf, n);
  free(buf);
}

// 字句 [s, e) の文字を、空白でつないで足す
static void omp_add_toks(OmpStr *s, Token *b, Token *e) {
  for (Token *t = b; t && t != e && t->kind != TK_EOF; t = t->next) {
    omp_add(s, " ");
    omp_addn(s, t->loc, t->len);
  }
}

static char *omp_tok_text(Token *b, Token *e) {
  OmpStr s = {0};
  omp_add_toks(&s, b, e);
  if (!s.p)
    omp_add(&s, "");
  return s.p;
}

// 文字列を、パーサーが読める字句の並びにする（キーワードの区別も）。終わりは TK_EOF
static Token *omp_tokenize(const char *src) {
  size_t n = strlen(src);
  char *buf = calloc(1, n + 2);
  memcpy(buf, src, n);
  buf[n] = '\n';
  // ファイルは pragma の字句のものを写して使う（new_file は前処理の領域に名前を足すが、その領域はもう捨ててある）
  File *f = calloc(1, sizeof(File));
  *f = *omp_file;
  f->contents = buf;
  Token *t = tokenize(f, NULL, NULL);
  // attr_next は字句を作るときの alloc_next と同じ場所なので、空にする（残すと、属性の並びに見えてしまう）
  for (Token *k = t;; k = k->next) {
    k->attr_next = NULL;
    if (k->kind == TK_EOF)
      break;
    if (k->kind == TK_IDENT)
      k->kind = ident_keyword(k);
  }
  return t;
}

// ---- 字句の並びを組み立てる（生成した文字と、元の字句をつなぐ）----

typedef struct {
  Token head;
  Token *tail;
} OmpBuild;

static void ob_init(OmpBuild *b) {
  b->head = (Token){0};
  b->tail = &b->head;
}

static void ob_text(OmpBuild *b, const char *src) {
  Token *t = omp_tokenize(src);
  for (; t->kind != TK_EOF; t = t->next)
    b->tail = b->tail->next = t;
}

// 元の字句 [s, e) をそのまま足す（つなぎ替えるので、同じ字句は 1 回だけ）
static void ob_range(OmpBuild *b, Token *s, Token *e) {
  for (Token *t = s; t != e; t = t->next)
    b->tail = b->tail->next = t;
}

static Token *ob_finish(OmpBuild *b, Token *cont) {
  b->tail->next = cont;
  return b->head.next;
}

// ---- 文の終わりを探す（字句で）----

static Token *omp_skip_braces(Token *tok) {
  int depth = 0;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (tok->kind == TK_LCURLY)
      depth++;
    else if (tok->kind == TK_RCURLY && --depth == 0)
      return tok->next;
  }
  error_tok(tok, "OpenMP: unterminated block");
}

static Token *omp_skip_line(Token *tok) {
  tok = tok->next;
  while (tok->kind != TK_EOF && !tok->at_bol)
    tok = tok->next;
  return tok;
}

static Token *omp_skip_stmt(Token *tok) {
  switch (tok->kind) {
  case TK_LCURLY:
    return omp_skip_braces(tok);
  case TK_PRAGMA:
    return omp_skip_stmt(omp_skip_line(tok));
  case TK_if: {
    tok = omp_skip_stmt(skip_paren(skip_tk(tok->next, TK_LPAREN)));
    if (tok->kind == TK_else)
      tok = omp_skip_stmt(tok->next);
    return tok;
  }
  case TK_for:
  case TK_while:
  case TK_switch:
    return omp_skip_stmt(skip_paren(skip_tk(tok->next, TK_LPAREN)));
  case TK_do:
    tok = omp_skip_stmt(tok->next);
    tok = skip_paren(skip_tk(skip_tk(tok, TK_while), TK_LPAREN));
    return skip_tk(tok, TK_SEMI);
  default:
    break;
  }
  if (tok->kind == TK_IDENT && tok->next->kind == TK_COLON)
    return omp_skip_stmt(tok->next->next);
  int depth = 0;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (tok->kind == TK_LPAREN || tok->kind == TK_LBRACK || tok->kind == TK_LCURLY)
      depth++;
    else if (tok->kind == TK_RPAREN || tok->kind == TK_RBRACK || tok->kind == TK_RCURLY)
      depth--;
    else if (tok->kind == TK_SEMI && depth == 0)
      return tok->next;
  }
  error_tok(tok, "OpenMP: expected a statement");
}

// ---- 指示と節 ----


#define OMP_MAX_VARS 32

typedef struct {
  int n;
  Token *names[OMP_MAX_VARS];
  char op[OMP_MAX_VARS][8];     // reduction の演算子（+ * - & | ^ && || max min）
} OmpVars;

typedef struct {
  char *num_threads, *if_expr;  // 式の文字（NULL ならなし）
  OmpVars priv, firstpriv, red;
  int schedule;                 // 0 static、1 dynamic
  char *chunk;
  bool nowait;
} OmpClauses;

static void omp_var_list(Token **rest, Token *tok, OmpVars *v, const char *op) {
  tok = skip_tk(tok, TK_LPAREN);
  while (tok->kind != TK_RPAREN) {
    if (tok->kind != TK_IDENT)
      error_tok(tok, "OpenMP: expected a variable name");
    if (v->n == OMP_MAX_VARS)
      error_tok(tok, "OpenMP: too many variables");
    v->names[v->n] = tok;
    snprintf(v->op[v->n], sizeof v->op[0], "%s", op ? op : "");
    v->n++;
    tok = tok->next;
    if (tok->kind == TK_COMMA)
      tok = tok->next;
  }
  *rest = tok->next;
}

// 括弧の中の字句の文字（clause(…) の …）
static char *omp_paren_text(Token **rest, Token *tok) {
  tok = skip_tk(tok, TK_LPAREN);
  Token *end = tok;
  int depth = 0;
  for (; end->kind != TK_EOF && !(end->kind == TK_RPAREN && depth == 0); end = end->next) {
    if (end->kind == TK_LPAREN)
      depth++;
    else if (end->kind == TK_RPAREN)
      depth--;
  }
  char *s = omp_tok_text(tok, end);
  *rest = end->next;
  return s;
}

// 指示の行の残り（tok から行の終わりまで）の節を読む
static void omp_clauses(Token *tok, OmpClauses *c) {
  while (tok->kind != TK_EOF && !tok->at_bol) {
    if (tok->kind == TK_COMMA) {
      tok = tok->next;
      continue;
    }
    if (equal(tok, "num_threads")) {
      c->num_threads = omp_paren_text(&tok, tok->next);
    } else if (tok->kind == TK_if || equal(tok, "if")) {
      c->if_expr = omp_paren_text(&tok, tok->next);
    } else if (equal(tok, "private") || equal(tok, "lastprivate")) {
      if (equal(tok, "lastprivate"))
        warn_tok(tok, "OpenMP: lastprivate is treated as private");
      omp_var_list(&tok, tok->next, &c->priv, NULL);
    } else if (equal(tok, "firstprivate")) {
      omp_var_list(&tok, tok->next, &c->firstpriv, NULL);
    } else if (equal(tok, "shared") || equal(tok, "default") || equal(tok, "collapse") || equal(tok, "proc_bind") ||
               equal(tok, "copyin") || equal(tok, "ordered") || equal(tok, "safelen") || equal(tok, "simdlen") ||
               equal(tok, "aligned") || equal(tok, "linear")) {
      if (equal(tok, "copyin"))
        error_tok(tok, "OpenMP: copyin is not supported");
      tok = tok->next;
      if (tok->kind == TK_LPAREN)
        tok = skip_paren(tok);
    } else if (equal(tok, "reduction")) {
      tok = skip_tk(tok->next, TK_LPAREN);
      char op[8] = "";
      if (equal(tok, "max") || equal(tok, "min"))
        snprintf(op, sizeof op, "%.*s", tok->len, tok->loc);
      else if (equal(tok, "+") || equal(tok, "-") || equal(tok, "*") || equal(tok, "&") || equal(tok, "|") ||
               equal(tok, "^") || equal(tok, "&&") || equal(tok, "||"))
        snprintf(op, sizeof op, "%.*s", tok->len, tok->loc);
      else
        error_tok(tok, "OpenMP: unsupported reduction operator");
      tok = skip_tk(tok->next, TK_COLON);
      // 名前の並び（) まで）
      while (tok->kind != TK_RPAREN) {
        if (tok->kind != TK_IDENT)
          error_tok(tok, "OpenMP: expected a variable name");
        if (c->red.n == OMP_MAX_VARS)
          error_tok(tok, "OpenMP: too many variables");
        c->red.names[c->red.n] = tok;
        snprintf(c->red.op[c->red.n], sizeof c->red.op[0], "%s", op);
        c->red.n++;
        tok = tok->next;
        if (tok->kind == TK_COMMA)
          tok = tok->next;
      }
      tok = tok->next;
    } else if (equal(tok, "schedule")) {
      tok = skip_tk(tok->next, TK_LPAREN);
      if (equal(tok, "monotonic") || equal(tok, "nonmonotonic")) {
        tok = skip_tk(tok->next, TK_COLON);
      }
      c->schedule = equal(tok, "dynamic") || equal(tok, "guided") ? 1 : 0;
      tok = tok->next;
      if (tok->kind == TK_COMMA) {
        Token *start = tok->next;
        Token *end = start;
        int depth = 0;
        for (; end->kind != TK_EOF && !(end->kind == TK_RPAREN && depth == 0); end = end->next) {
          if (end->kind == TK_LPAREN)
            depth++;
          else if (end->kind == TK_RPAREN)
            depth--;
        }
        c->chunk = omp_tok_text(start, end);
        tok = end;
      }
      tok = skip_tk(tok, TK_RPAREN);
    } else if (equal(tok, "nowait")) {
      c->nowait = true;
      tok = tok->next;
    } else {
      error_tok(tok, "OpenMP: unsupported clause");
    }
  }
}

static const char omp_decls[] =
    "void __monocs_omp_parallel(void (*)(void **), void **, int, int);"
    "long long __monocs_omp_trip(long long, long long, long long, int);"
    "void __monocs_omp_loop_init(long long *, int, long long, long long);"
    "int __monocs_omp_loop_next(long long *, long long *, long long *);"
    "void __monocs_omp_loop_end(long long *, int);"
    "void __monocs_omp_barrier(void);"
    "int __monocs_omp_single(void);"
    "void __monocs_omp_lock(void);"
    "void __monocs_omp_unlock(void);"
    "void __monocs_omp_flush(void);"
    "int omp_get_thread_num(void);";

static char *omp_name(Token *t) {
  return arena_format(&ast_arena, "%.*s", t->len, t->loc);
}

// reduction の始めの値（max・min・& は、元の値から始める。結果は同じ）
static void omp_red_init(OmpStr *s, const char *op, const char *v, const char *rp) {
  if (!strcmp(op, "*") || !strcmp(op, "&&"))
    omp_addf(s, " __typeof__(%s) %s = 1;", v, v);
  else if (!strcmp(op, "max") || !strcmp(op, "min") || !strcmp(op, "&"))
    omp_addf(s, " __typeof__(%s) %s = *%s;", v, v, rp);
  else
    omp_addf(s, " __typeof__(%s) %s = 0;", v, v);
}

static void omp_red_combine(OmpStr *s, const char *op, const char *v, const char *rp) {
  if (!strcmp(op, "max"))
    omp_addf(s, " if (%s > *%s) *%s = %s;", v, rp, rp, v);
  else if (!strcmp(op, "min"))
    omp_addf(s, " if (%s < *%s) *%s = %s;", v, rp, rp, v);
  else if (!strcmp(op, "-"))
    omp_addf(s, " *%s = *%s + %s;", rp, rp, v);
  else
    omp_addf(s, " *%s = *%s %s %s;", rp, rp, op, v);
}

// 節の private・firstprivate・reduction の、始め（out に。外側のブロックの前置き pre と、内側のブロックの
// 宣言 in に分ける）と終わり（post）の字句の文字
static void omp_data_clauses(OmpClauses *c, int id, OmpStr *pre, OmpStr *in, OmpStr *post) {
  for (int i = 0; i < c->firstpriv.n; i++) {
    char *v = omp_name(c->firstpriv.names[i]);
    omp_addf(pre, " __typeof__(%s) __omp_fp%d_%s = %s;", v, id, v, v);
    omp_addf(in, " __typeof__(%s) %s = __omp_fp%d_%s;", v, v, id, v);
  }
  for (int i = 0; i < c->priv.n; i++) {
    char *v = omp_name(c->priv.names[i]);
    omp_addf(in, " __typeof__(%s) %s;", v, v);
  }
  if (c->red.n) {
    for (int i = 0; i < c->red.n; i++) {
      char *v = omp_name(c->red.names[i]);
      omp_addf(pre, " __typeof__(%s) *__omp_rp%d_%s = &%s;", v, id, v, v);
    }
    for (int i = 0; i < c->red.n; i++) {
      char *v = omp_name(c->red.names[i]);
      char rp[160];
      snprintf(rp, sizeof rp, "__omp_rp%d_%s", id, v);
      omp_red_init(in, c->red.op[i], v, rp);
    }
    omp_add(post, " __monocs_omp_lock();");
    for (int i = 0; i < c->red.n; i++) {
      char *v = omp_name(c->red.names[i]);
      char rp[160];
      snprintf(rp, sizeof rp, "__omp_rp%d_%s", id, v);
      omp_red_combine(post, c->red.op[i], v, rp);
    }
    omp_add(post, " __monocs_omp_unlock();");
  }
}

// ---- #pragma omp for: 繰り返しを書き換える ----

// for (INIT; COND; INC) BODY の字句を、受け持ちの範囲だけを回す形にして b に足す（BODY は元の字句）。
// tok は for。*rest は文の次
static void omp_for_loop(OmpBuild *b, Token **rest, Token *tok, OmpClauses *c, bool in_parallel_for) {
  int id = ++omp_counter;
  if (tok->kind != TK_for)
    error_tok(tok, "OpenMP: expected a for loop");
  Token *lp = skip_tk(tok->next, TK_LPAREN);
  Token *rp = skip_paren(lp);            // ) の次
  // INIT: [型] VAR = LB
  Token *semi1 = lp;
  for (int depth = 0; !(semi1->kind == TK_SEMI && depth == 0); semi1 = semi1->next) {
    if (semi1->kind == TK_EOF)
      error_tok(tok, "OpenMP: bad for loop");
    if (semi1->kind == TK_LPAREN)
      depth++;
    else if (semi1->kind == TK_RPAREN)
      depth--;
  }
  Token *semi2 = semi1->next;
  for (int depth = 0; !(semi2->kind == TK_SEMI && depth == 0); semi2 = semi2->next) {
    if (semi2->kind == TK_EOF)
      error_tok(tok, "OpenMP: bad for loop");
    if (semi2->kind == TK_LPAREN)
      depth++;
    else if (semi2->kind == TK_RPAREN)
      depth--;
  }
  Token *assign = lp;
  while (assign != semi1 && !equal(assign, "="))
    assign = assign->next;
  if (assign == semi1 || assign == lp)
    error_tok(lp, "OpenMP: the loop must start with VAR = expression");
  Token *var = lp;
  while (var->next != assign)
    var = var->next;
  if (var->kind != TK_IDENT)
    error_tok(var, "OpenMP: expected the loop variable");
  bool decl = var != lp;        // for (int i = …)
  char *vname = omp_name(var);
  char *type_text = decl ? omp_tok_text(lp, var) : NULL;
  char *lb = omp_tok_text(assign->next, semi1);
  // COND: VAR op UB か UB op VAR
  Token *ct = semi1->next;
  int op;
  char *ub;
  static const char *const ops[] = {"<", "<=", ">", ">=", "!="};
  Token *optok = NULL;
  if (ct->kind == TK_IDENT && equal_tok(ct, var)) {
    optok = ct->next;
    ub = omp_tok_text(optok->next, semi2);
  } else {
    optok = ct;
    for (int depth = 0; optok != semi2; optok = optok->next) {
      if (optok->kind == TK_LPAREN)
        depth++;
      else if (optok->kind == TK_RPAREN)
        depth--;
      else if (depth == 0 && optok->next != semi2 && optok->next->kind == TK_IDENT && equal_tok(optok->next, var) &&
               optok->next->next == semi2)
        break;
    }
    if (optok == semi2)
      error_tok(ct, "OpenMP: the loop condition must compare the loop variable");
    ub = omp_tok_text(ct, optok);
  }
  op = -1;
  for (int i = 0; i < 5; i++)
    if (equal(optok, ops[i]))
      op = i;
  if (op < 0)
    error_tok(optok, "OpenMP: the loop condition must be < <= > >= or !=");
  if (optok == ct || !(ct->kind == TK_IDENT && equal_tok(ct, var))) {
    static const int flip[] = {2, 3, 0, 1, 4};   // a < i は i > a
    op = flip[op];
  }
  // INC: VAR++ ++VAR VAR-- --VAR VAR += E VAR -= E VAR = VAR + E VAR = VAR - E VAR = E + VAR
  Token *it = semi2->next;
  Token *rparen = it;
  for (int depth = 0;; rparen = rparen->next) {
    if (rparen->kind == TK_LPAREN)
      depth++;
    else if (rparen->kind == TK_RPAREN && depth-- == 0)
      break;
  }
  char *step = NULL;
  if (it->kind == TK_IDENT && equal_tok(it, var) && (equal(it->next, "++") || equal(it->next, "--")) &&
      it->next->next == rparen)
    step = equal(it->next, "++") ? "1" : "-1";
  else if ((equal(it, "++") || equal(it, "--")) && it->next->kind == TK_IDENT && equal_tok(it->next, var) &&
           it->next->next == rparen)
    step = equal(it, "++") ? "1" : "-1";
  else if (it->kind == TK_IDENT && equal_tok(it, var) && (equal(it->next, "+=") || equal(it->next, "-="))) {
    char *e = omp_tok_text(it->next->next, rparen);
    step = arena_format(&ast_arena, equal(it->next, "+=") ? "(%s)" : "-(%s)", e);
  } else if (it->kind == TK_IDENT && equal_tok(it, var) && equal(it->next, "=")) {
    Token *r = it->next->next;
    if (r->kind == TK_IDENT && equal_tok(r, var) && (equal(r->next, "+") || equal(r->next, "-"))) {
      char *e = omp_tok_text(r->next->next, rparen);
      step = arena_format(&ast_arena, equal(r->next, "+") ? "(%s)" : "-(%s)", e);
    } else {
      // VAR = E + VAR
      Token *plus = r;
      while (plus != rparen && !(equal(plus, "+") && plus->next->kind == TK_IDENT && equal_tok(plus->next, var) &&
                                 plus->next->next == rparen))
        plus = plus->next;
      if (plus != rparen)
        step = arena_format(&ast_arena, "(%s)", omp_tok_text(r, plus));
    }
  }
  if (!step)
    error_tok(it, "OpenMP: the loop increment must be ++, --, += or -= of the loop variable");
  Token *body = rp;
  Token *end = omp_skip_stmt(body);
  *rest = end;

  OmpStr pre = {0}, in = {0}, post = {0}, s = {0};
  omp_data_clauses(c, id, &pre, &in, &post);
  omp_addf(&s, "{ %s", omp_decls);
  if (decl)
    omp_addf(&s, " %s %s;", type_text, vname);
  else
    omp_addf(&s, " __typeof__(%s) %s;", vname, vname);   // 繰り返しの変数は、スレッドごと（private）
  omp_addf(&s,
           " long long __omp_lb%d = (long long)(%s), __omp_ub%d = (long long)(%s), __omp_st%d = (long long)(%s);"
           " long long __omp_it%d[6], __omp_from%d, __omp_to%d;"
           " __monocs_omp_loop_init(__omp_it%d, %d, %s, __monocs_omp_trip(__omp_lb%d, __omp_ub%d, __omp_st%d, %d));",
           id, lb, id, ub, id, step, id, id, id, id, c->schedule, c->chunk ? c->chunk : "0", id, id, id, op);
  omp_add(&s, pre.p ? pre.p : "");
  omp_addf(&s, " { %s", in.p ? in.p : "");
  omp_addf(&s,
           " while (__monocs_omp_loop_next(__omp_it%d, &__omp_from%d, &__omp_to%d))"
           " for (long long __omp_k%d = __omp_from%d; __omp_k%d < __omp_to%d; __omp_k%d++)"
           " for (int __omp_once%d = (%s = (__typeof__(%s))(__omp_lb%d + __omp_k%d * __omp_st%d), 1);"
           " __omp_once%d; __omp_once%d = 0)",
           id, id, id, id, id, id, id, id, id, vname, vname, id, id, id, id, id);
  ob_text(b, s.p);
  ob_range(b, body, end);
  OmpStr tail = {0};
  omp_addf(&tail, " %s }", post.p ? post.p : "");
  omp_addf(&tail, " __monocs_omp_loop_end(__omp_it%d, %d); }", id, c->nowait && !in_parallel_for ? 1 : 0);
  ob_text(b, tail.p);
  free(pre.p);
  free(in.p);
  free(post.p);
  free(s.p);
  free(tail.p);
}

// ---- #pragma omp parallel: 文を関数に切り出す ----

typedef struct OmpOutline OmpOutline;
struct OmpOutline {
  OmpOutline *outer;
  Scope *base;          // 切り出した関数の、いちばん外のスコープ
  Obj *ctx;             // 引数 void **ctx
  int n, cap;
  Obj **vars;           // 外の関数の変数
  Obj **ptrs;           // それを指すポインタ（切り出した関数の局所変数）
  Token **names;
};

TLS static OmpOutline *omp_outline;

// 変数への参照（primary から）。切り出した関数の中から外の関数の局所変数を見つけたら、ポインタを通す
static Node *omp_ref(Obj *var, Token *tok, bool past_base) {
  if (!omp_outline || !past_base || var->kind != OBJ_LOCAL)
    return new_var_node(var, tok);
  OmpOutline *o = omp_outline;
  for (int i = 0; i < o->n; i++)
    if (o->vars[i] == var)
      return new_unary(ND_DEREF, new_var_node(o->ptrs[i], tok), tok);
  if (var->ty->kind == TY_VLA)
    error_tok(tok, "OpenMP: a variable length array declared outside the parallel region can't be used in it");
  if (o->n == o->cap) {
    o->cap = o->cap ? o->cap * 2 : 16;
    o->vars = realloc(o->vars, sizeof(Obj *) * o->cap);
    o->ptrs = realloc(o->ptrs, sizeof(Obj *) * o->cap);
    o->names = realloc(o->names, sizeof(Token *) * o->cap);
  }
  Obj *p = new_lvar2(pointer_to(var->ty), o->base);
  o->vars[o->n] = var;
  o->ptrs[o->n] = p;
  o->names[o->n] = tok;
  o->n++;
  return new_unary(ND_DEREF, new_var_node(p, tok), tok);
}

// body の字句（{ … } の形。終わりは TK_EOF）を、新しい関数として読んで出力する。呼び出しのコードを返す
static Node *omp_outline_region(Token *body, Token *at, char *num_threads, char *if_expr, Token *cont,
                                Token **rest) {
  int id = ++omp_counter;
  char *name = arena_format(&cc1_arena, "__omp_fn%d", id);
  Type *gty = func_type(TY_FUNC, ty_void);
  Obj *g = new_gvar(name, gty);
  g->is_static = true;
  g->is_definition = true;

  // 今のパーサーの状態を控える
  Scope *saved_scope = scope;
  FuncContext *saved_fnctx = fnctx;
  JumpContext *saved_jump = jump_ctx;

  enter_fn_scope(gty);      // 親は今のスコープ（外の関数の型・タグ・列挙の名前も見える）
  Scope *base = scope;
  Obj *ctx = new_param(NULL, pointer_to(pointer_to(ty_void)));
  gty->param_list = ctx;

  OmpOutline o = {.outer = omp_outline, .base = base, .ctx = ctx};
  omp_outline = &o;
  FuncContext gctx = {.fn = g};
  fnctx = &gctx;
  jump_ctx = NULL;

  Token *end;
  g->body = compound_stmt2(&end, skip_tk(body, TK_LCURLY), ND_BLOCK);    // 最後に leave_scope で base を出る
  // ポインタを ctx から受け取る文を、前に置く
  Node head = {0}, *cur = &head;
  for (int i = 0; i < o.n; i++) {
    Node *slot = new_unary(ND_DEREF, new_add(new_var_node(ctx, at), new_num(i, at), at), at);
    Node *asg = new_binary(ND_ASSIGN, new_var_node(o.ptrs[i], at), new_cast(slot, o.ptrs[i]->ty), at);
    cur = cur->next = new_unary(ND_EXPR_STMT, asg, at);
  }
  cur->next = g->body->blk.body;
  g->body->blk.body = head.next;
  add_type(g->body);
  if (gctx.use_vla && !gctx.dont_dealloc_vla && (opt_reuse_stack && !g->dont_reuse_stk))
    g->dealloc_vla = true;
  if (gctx.gotos)
    resolve_gotos(g->body);

  omp_outline = o.outer;
  free(gctx.strlits.buckets);
  fnctx = saved_fnctx;
  scope = saved_scope;
  jump_ctx = saved_jump;
  emit_text(g);

  // 呼び出しのコード: { void *ctx[n]; ctx[i] = &v; …; __monocs_omp_parallel(関数, ctx, スレッドの数, if); }
  push_var_name2(name, strlen(name), at, g);
  OmpStr s = {0};
  int n = o.n ? o.n : 1;
  omp_addf(&s, "{ %s void *__omp_ctx%d[%d];", omp_decls, id, n);
  for (int i = 0; i < o.n; i++)
    omp_addf(&s, " __omp_ctx%d[%d] = (void *)&%.*s;", id, i, o.names[i]->len, o.names[i]->loc);
  omp_addf(&s, " __monocs_omp_parallel(%s, __omp_ctx%d, %s, %s); }", name, id, num_threads ? num_threads : "0",
           if_expr ? if_expr : "1");
  free(o.vars);
  free(o.ptrs);
  free(o.names);
  OmpBuild b;
  ob_init(&b);
  ob_text(&b, s.p);
  free(s.p);
  Token *call = ob_finish(&b, cont);
  return stmt(rest, call, NULL);
}

// ---- 指示 ----

// #pragma omp …（tok は TK_PRAGMA の omp）と、その次の文。置き換えた文を返す（なければ NULL）
static Node *omp_directive(Token **rest, Token *tok) {
  omp_file = tok->file;
  Token *line_end = omp_skip_line(tok);
  Token *d = tok->next;
  Token *stmt_tok = line_end;
  OmpClauses c = {0};

  if (equal(d, "parallel")) {
    Token *k = d->next;
    bool is_for = false;
    if (k->kind == TK_for || equal(k, "for")) {
      is_for = true;
      k = k->next;
      if (equal(k, "simd"))
        k = k->next;
    } else if (equal(k, "sections")) {
      error_tok(k, "OpenMP: sections is not supported");
    }
    omp_clauses(k, &c);
    // 本文: { [firstprivate の受け取り] { [private などの宣言] 文 [reduction] } }
    Token *end;
    OmpBuild b;
    ob_init(&b);
    if (is_for) {
      // parallel for: 節は for の側で扱う（num_threads・if は parallel）
      ob_text(&b, "{");
      omp_for_loop(&b, &end, stmt_tok, &c, true);
      ob_text(&b, "}");
    } else {
      end = omp_skip_stmt(stmt_tok);
      OmpStr pre = {0}, in = {0}, post = {0}, s = {0};
      omp_data_clauses(&c, ++omp_counter, &pre, &in, &post);
      omp_addf(&s, "{ %s %s { %s", omp_decls, pre.p ? pre.p : "", in.p ? in.p : "");
      ob_text(&b, s.p);
      ob_range(&b, stmt_tok, end);
      OmpStr t = {0};
      omp_addf(&t, " %s } }", post.p ? post.p : "");
      ob_text(&b, t.p);
      free(pre.p);
      free(in.p);
      free(post.p);
      free(s.p);
      free(t.p);
    }
    Token *eof = omp_tokenize("");
    Token *body = ob_finish(&b, eof);

    return omp_outline_region(body, d, c.num_threads, c.if_expr, end, rest);
  }

  if (d->kind == TK_for || equal(d, "for")) {
    Token *k = d->next;
    if (equal(k, "simd"))
      k = k->next;
    omp_clauses(k, &c);
    Token *end;
    OmpBuild b;
    ob_init(&b);
    omp_for_loop(&b, &end, stmt_tok, &c, false);
    Token *t = ob_finish(&b, end);
    return stmt(rest, t, NULL);
  }

  if (equal(d, "critical") || equal(d, "atomic")) {
    Token *end = omp_skip_stmt(stmt_tok);
    OmpBuild b;
    ob_init(&b);
    OmpStr s = {0};
    omp_addf(&s, "{ %s __monocs_omp_lock();", omp_decls);
    ob_text(&b, s.p);
    free(s.p);
    ob_range(&b, stmt_tok, end);
    ob_text(&b, " __monocs_omp_unlock(); }");
    return stmt(rest, ob_finish(&b, end), NULL);
  }

  if (equal(d, "single") || equal(d, "master") || equal(d, "masked")) {
    omp_clauses(d->next, &c);
    bool single = equal(d, "single");
    Token *end = omp_skip_stmt(stmt_tok);
    OmpBuild b;
    ob_init(&b);
    OmpStr s = {0};
    omp_addf(&s, "{ %s if (%s)", omp_decls, single ? "__monocs_omp_single()" : "omp_get_thread_num() == 0");
    ob_text(&b, s.p);
    free(s.p);
    ob_range(&b, stmt_tok, end);
    ob_text(&b, single && !c.nowait ? " __monocs_omp_barrier(); }" : " }");
    return stmt(rest, ob_finish(&b, end), NULL);
  }

  if (equal(d, "task"))       // その場で動かす（OpenMP が認める、遅らせない task と同じ）
    return stmt(rest, stmt_tok, NULL);

  const char *call = NULL;
  if (equal(d, "barrier"))
    call = "__monocs_omp_barrier();";
  else if (equal(d, "flush"))
    call = "__monocs_omp_flush();";
  else if (equal(d, "taskwait") || equal(d, "taskyield"))
    call = ";";
  if (call) {
    OmpBuild b;
    ob_init(&b);
    OmpStr s = {0};
    omp_addf(&s, "{ %s %s }", omp_decls, call);
    ob_text(&b, s.p);
    free(s.p);
    return stmt(rest, ob_finish(&b, line_end), NULL);
  }

  if (equal(d, "simd") || equal(d, "declare") || equal(d, "ordered") || equal(d, "end") || equal(d, "requires")) {
    *rest = line_end;       // 読み捨てる（次の文はそのまま）
    return NULL;
  }
  if (equal(d, "threadprivate"))
    error_tok(d, "OpenMP: threadprivate is not supported");
  error_tok(d, "OpenMP: unsupported directive");
}

// 前のコンパイルが途中で（エラーで）抜けたときの状態を消す（libslimcc。parse_reset から）
static void omp_reset(void) {
  omp_outline = NULL;
  omp_counter = 0;
}
