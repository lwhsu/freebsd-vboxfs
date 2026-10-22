#!/bin/sh
# Classify failed pjdfstest assertions by operation and expected/got errno.
# usage: pjd-failures.sh RESULTS_DIR  -> writes failures.tsv, prints buckets
R=${1:?results dir}
cd "$R" || exit 1
find log -name '*.t.log' | sort | while read -r f; do
	t=$(echo "$f" | sed -e 's|^log/||' -e 's|^\./||' -e 's|\.log$||')
	awk -v t="$t" -v q="'" '
	/^not ok [0-9]+/ {
		n = $3; op = "?"; ex = "?"; got = "?";
		re = "tried " q "[^" q "]*" q ", expected [^,]+, got .*$";
		if (match($0, re)) {
			s = substr($0, RSTART, RLENGTH);
			sub("^tried " q, "", s);
			op = s; sub("[ " q "].*$", "", op);
			ex = s; sub("^[^" q "]*" q ", expected ", "", ex); sub(", got .*$", "", ex);
			got = s; sub("^.*, got ", "", got);
		} else {
			got = $0; sub(/^not ok [0-9]+ ?-? ?/, "", got);
		}
		printf("%s\t%s\t%s\t%s\t%s\n", t, n, op, ex, got);
	}' "$f"
done > failures.tsv
echo "total failed assertions: $(wc -l < failures.tsv | tr -d ' ')"
echo
echo "== by operation =="
awk -F'\t' '{ c[$3]++ } END { for (k in c) printf("%6d  %s\n", c[k], k) }' failures.tsv | sort -rn
echo
echo "== top (op, expected, got) =="
awk -F'\t' '{ c[$3 "\t" $4 "\t" $5]++ } END { for (k in c) printf("%6d  %s\n", c[k], k) }' failures.tsv | sort -rn | head -40
