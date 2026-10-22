#!/bin/sh
# tools/style-check.sh flags mixed -> and . chains and literal true/false
# arguments, and leaves allowed forms alone.
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
# The format rule is checked by the real tree run; skip it for these samples.
export CLANG_FORMAT=true

printf '%s\n' 'void f(void)' '{' '	x = a->b.c;' '	y = s.p->q;' '	g(a, true);' '}' >"$work/bad.c"
if out=$(sh tools/style-check.sh "$work/bad.c"); then
	echo "style-check passed a file with violations" >&2
	exit 1
fi
[ "$(printf '%s\n' "$out" | grep -c 'mixed -> and . chain')" -eq 2 ]
[ "$(printf '%s\n' "$out" | grep -c 'literal true/false argument')" -eq 1 ]

printf '%s\n' 'bool f(struct s *p)' '{' '	struct t *t = &p->t;' '	t->n = p->n;' \
	'	h(1, false); /* style: allow */' '	/* a->b.c in a comment */' '	return true;' '}' >"$work/good.c"
sh tools/style-check.sh "$work/good.c" >/dev/null
echo 'style check tests passed'
