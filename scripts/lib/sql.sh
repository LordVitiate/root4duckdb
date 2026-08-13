#!/usr/bin/env bash

# Escapes a value for a single-quoted SQL literal.
root4duckdb_sql_escape() {
    printf '%s' "$1" | sed "s/'/''/g"
}
