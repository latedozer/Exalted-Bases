#!/bin/bash
# Builds the migration tool natively (it runs with the game closed, on this machine).
set -e
cd "$(dirname "$0")"
mkdir -p ../build/native
g++ -std=c++20 -O2 -Wall -Wextra -o ../build/native/exalted_migrate migrate.cpp ../exalted_core.cpp ../save_reader.cpp
echo "built ../build/native/exalted_migrate"
