-- complain if the script is sourced in psql instead of CREATE EXTENSION
\echo Use "CREATE EXTENSION pgtier" to load this file. \quit

CREATE FUNCTION pgtier_hello()
RETURNS text
AS 'MODULE_PATHNAME', 'pgtier_hello'
LANGUAGE C STRICT;
