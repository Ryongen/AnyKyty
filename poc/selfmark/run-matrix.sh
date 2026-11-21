#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
b="$here/out/selfmark"
echo "== A calé sur les envois de Yōtei : 11 500 pages par image, 11 fils, une synchro par image"; "$b" --frames 60
echo "== B calé sur les fautes de Yōtei : 2 300 pages par image"; "$b" --frames 60 --pages 2300
echo "== C blocs de 64 Kio pour le bras fautes"; "$b" --frames 60 --block 65536 --modes none,fault
echo "== D 60 points de synchro par image"; "$b" --frames 30 --syncs 60
echo "== E 587 points de synchro par image (soumissions de Yōtei)"; "$b" --frames 20 --syncs 587
echo "== F un seul fil écrivain"; "$b" --frames 60 --threads 1 --pages 2300
echo "== G mémoire suivie de 16 Gio (balayage)"; "$b" --frames 30 --region-mib 16384 --syncs 60 --modes uffd-scan,self-mark
