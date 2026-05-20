#!/usr/bin/env bash
set -e
cd /mnt/c/Users/makss/Desktop/scoped/projects/mdsys
git add src/main.cpp
git commit -m "feat: auto-pin service after registration"
git push origin main
rm -- "$0"
