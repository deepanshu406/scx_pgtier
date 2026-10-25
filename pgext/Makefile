# SPDX-License-Identifier: GPL-2.0
#
# Build the pgtier PostgreSQL extension with PGXS.
#
#   make                      build pgtier.so
#   sudo make install         copy it, the .control and .sql files into PostgreSQL
#   make PG_CONFIG=/path/to/pg_config   use a different PostgreSQL
#   make clean

EXTENSION = pgtier
MODULES   = pgtier
DATA      = pgtier--1.0.sql

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
