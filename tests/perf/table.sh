#!/bin/sh
# Render a markdown table comparing several bench.sh CSV files (columns in
# the order given).  usage: table.sh a.csv b.csv ...
awk -F',' '
	FNR == 1 { ncol++; col[ncol] = FILENAME; sub(/.*\//, "", col[ncol]); sub(/\.csv$/, "", col[ncol]) }
	$2 != "done" { key = $2 "," $4; if (!(key in seen)) { seen[key] = 1; order[++n] = key }; v[key, ncol] = $3 }
	END {
		printf "| test | unit"; for (c = 1; c <= ncol; c++) printf " | %s", col[c]; printf " |\n"
		printf "|---|---"; for (c = 1; c <= ncol; c++) printf "|---"; printf "|\n"
		for (i = 1; i <= n; i++) { split(order[i], k, ","); printf "| %s | %s", k[1], k[2]; for (c = 1; c <= ncol; c++) printf " | %s", ((order[i], c) in v) ? v[order[i], c] : "-"; printf " |\n" }
	}' "$@"
