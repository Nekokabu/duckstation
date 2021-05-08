#!/bin/sh

if [ $# -ne 1 ]; then
  echo "Usage: $0 <key>"
  exit 1
fi

IFS="
"

ls *.cpp *.h *.py 1>/dev/null 2>/dev/null
if [ $? -ne 0 ]; then
  echo "No unsealed files"
  exit 1
fi

for i in *.cpp *.h *.py; do
  NEWNAME="${i}.gpg"
  echo "${i} -> ${NEWNAME}"
  rm -f "${i}.gpg"
  gpg --batch --passphrase "$1" -c $i
done
