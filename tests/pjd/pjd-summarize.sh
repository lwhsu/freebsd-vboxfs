#!/bin/sh
# Summarize a pjdfstest run: per .t file counts + per-directory table.
# usage: pjd-summarize.sh RESULTS_DIR   (expects RESULTS_DIR/log/<dir>/<n>.t.log and RESULTS_DIR/rc.txt)
R=${1:?results dir}
cd "$R" || exit 1
echo "file	rc	planned	ok	not_ok	failed_assertions" > per-file.tsv
sort rc.txt | while read -r t rc; do
	log="log/$t.log"
	if [ ! -f "$log" ]; then echo "$t	$rc	-	-	-	(no log)" >> per-file.tsv; continue; fi
	planned=$(awk '/^1\.\.[0-9]+$/ { sub("1\\.\\.", ""); print; exit }' "$log")
	ok=$(/usr/bin/grep -cE '^ok [0-9]+' "$log")
	nok=$(/usr/bin/grep -cE '^not ok [0-9]+' "$log")
	failed=$(/usr/bin/grep -oE '^not ok [0-9]+' "$log" | awk '{print $3}' | tr '\n' ',' | sed 's/,$//')
	echo "$t	$rc	${planned:--}	$ok	$nok	$failed" >> per-file.tsv
done
echo
echo "| directory | files | planned | ok | not ok | files w/ failures | timeout(137) | interrupted |"
echo "|---|---|---|---|---|---|---|---|"
tail -n +2 per-file.tsv | awk -F'\t' '{ split($1, a, "/"); print a[2] }' | sort -u | while read -r d; do
	tail -n +2 per-file.tsv | awk -F'\t' -v d="$d" '
	{ split($1, a, "/"); if (a[2] != d) next;
	  files++; if ($3 != "-") planned += $3; ok += $4; nok += $5;
	  if ($5 > 0 || ($2 != 0 && $2 != "INTERRUPTED")) ff++;
	  if ($2 == 137) to++; if ($2 == "INTERRUPTED") intr++ }
	END { printf("| %s | %d | %d | %d | %d | %d | %d | %d |\n", d, files, planned, ok, nok, ff, to, intr) }'
done
tail -n +2 per-file.tsv | awk -F'\t' '
{ files++; if ($3 != "-") planned += $3; ok += $4; nok += $5;
  if ($5 > 0 || ($2 != 0 && $2 != "INTERRUPTED")) ff++;
  if ($2 == 137) to++; if ($2 == "INTERRUPTED") intr++ }
END { printf("| **total** | %d | %d | %d | %d | %d | %d | %d |\n", files, planned, ok, nok, ff, to, intr) }'
