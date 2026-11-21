#!/bin/sh
# Monocs の sh と bash で同じ出力になること（tools/test/shell.sh。lines.sh が sh script.sh x y で動かす）
echo "args: $# [$1] [$2]"
count() {
	n=0
	for x in "$@"; do
		n=$((n + 1))
	done
	echo "count=$n"
}
count a 'b c' d
cat <<-END
	tab stripped $((2 + 3))
	END
case "$1" in
	x*) echo "starts with x" ;;
	*) echo other ;;
esac
i=0
while :; do
	i=$((i + 1))
	[ $i -ge 3 ] && break
done
echo "i=$i"
for f in one two; do
	case $f in
		one) continue ;;
	esac
	echo "f=$f"
done
v=${UNSET_MONOCS:-fallback}
echo "v=$v ${#v}"
echo "$(echo nested $(echo deep))"
if [ -n "$2" ] && [ "$2" != z ]; then
	echo "second=$2"
fi
helper=from-script
