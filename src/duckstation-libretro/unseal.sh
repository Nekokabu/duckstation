#!/bin/sh

if [ $# -ne 1 ]; then
  echo "Usage: $0 <key>"
  exit 1
fi

IFS="
"

if [ $1 = "unseal" ]; then
    ls *.gpg 1>/dev/null 2>/dev/null
    if [ $? -ne 0 ]; then
      echo "No sealed files"
      exit 1
    fi

    for i in *.gpg; do
      ORIGNAME=$(echo $i | sed -e 's/\.gpg$//')
      echo "${i} -> ${ORIGNAME}"
      rm -f $ORIGNAME
      gpg --batch --passphrase "$1" $i
    done
fi
