// C の Web（docs/cweb.md）: ページ（サンドボックスで動く C のプログラム）から、ほかのページへ移る・戻る。
// ブラウザは シェルの cweb URL。リンクは今のページからの相対（"next.c"、"sub/x.c"）か、http://… か /… 。
// "#sha256=…" を付けると、移った先の中身を確かめる（合わなければ動かさない）。
#pragma once

[[noreturn]] void cweb_go(const char *url);     // このページを終えて url へ
[[noreturn]] void cweb_back(void);              // このページを終えて、1 つ前のページへ
