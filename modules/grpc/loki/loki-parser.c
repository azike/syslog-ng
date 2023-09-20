/*
 * Copyright (c) 2023 László Várady
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "driver.h"
#include "cfg-parser.h"
#include "loki-grammar.h"

extern int loki_debug;

int loki_parse(CfgLexer *lexer, LogDriver **instance, gpointer arg);

static CfgLexerKeyword loki_keywords[] =
{
  { "loki", KW_LOKI },
  { "url", KW_URL },
  { "labels", KW_LABELS },
  { "timestamp", KW_TIMESTAMP },
  { "current", KW_CURRENT },
  { "received", KW_RECEIVED },
  { "msg", KW_MSG },
  { "keep_alive", KW_KEEP_ALIVE },
  { "time", KW_TIME },
  { "timeout", KW_TIMEOUT },
  { "max_pings_without_data", KW_MAX_PINGS_WITHOUT_DATA },
  { NULL }
};

CfgParser loki_parser =
{
#if SYSLOG_NG_ENABLE_DEBUG
  .debug_flag = &loki_debug,
#endif
  .name = "loki",
  .keywords = loki_keywords,
  .parse = (gint (*)(CfgLexer *, gpointer *, gpointer)) loki_parse,
  .cleanup = (void (*)(gpointer)) log_pipe_unref,
};

CFG_PARSER_IMPLEMENT_LEXER_BINDING(loki_, LOKI_, LogDriver **)
