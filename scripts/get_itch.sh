#!/bin/sh
# Downloads one of the sample days NASDAQ publishes and unpacks it into data/.
# About 3.5 GB compressed and 8.3 GB unpacked.
set -e
day=${1:-12302019}
file="$day.NASDAQ_ITCH50"
mkdir -p data
cd data
if [ ! -f "$file" ]; then
    curl -L --fail -C - -o "$file.gz" "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/$file.gz"
    gunzip -k "$file.gz"
fi
ls -l "$file"
