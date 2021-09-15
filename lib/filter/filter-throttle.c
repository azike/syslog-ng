/*
 * Copyright (c) 2021 One Identity
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "filter-throttle.h"
#include "timeutils/misc.h"
#include "scratch-buffers.h"
#include "str-utils.h"

typedef struct _FilterThrottle
{
  FilterExprNode super;
  NVHandle key_handle;
  gint rate;
  GMutex *map_lock;
  GHashTable *rate_limits;
} FilterThrottle;

typedef struct _ThrottleRateLimit
{
  gint tokens;
  gint rate;
  GTimeVal last_check;
  GMutex *lock;
} ThrottleRateLimit;

static ThrottleRateLimit *
throttle_ratelimit_new(gint rate)
{
  ThrottleRateLimit *self = g_new0(ThrottleRateLimit, 1);

  GTimeVal now;
  g_get_current_time(&now);
  self->last_check = now;
  self->lock = g_mutex_new();
  self->rate = rate;
  self->tokens = rate;

  return self;
}

static void
throttle_ratelimit_free(ThrottleRateLimit *self)
{
  g_mutex_free(self->lock);
  g_free(self);
}

static void
throttle_ratelimit_add_new_tokens(ThrottleRateLimit *self)
{
  glong usec_since_last_fill;
  gint num_new_tokens;
  GTimeVal now;
  g_get_current_time(&now);
  usec_since_last_fill = g_time_val_diff(&now, &self->last_check);

  num_new_tokens = (usec_since_last_fill * self->rate) / G_USEC_PER_SEC;
  if (num_new_tokens)
    {
      g_mutex_lock(self->lock);
      {
        self->tokens = MIN(self->rate, self->tokens + num_new_tokens);
        self->last_check = now;
      }
      g_mutex_unlock(self->lock);
    }
}

static gboolean
throttle_ratelimit_try_consume_tokens(ThrottleRateLimit *self, gint num_tokens)
{
  gboolean within_ratelimit;
  g_mutex_lock(self->lock);
  {
    if (self->tokens >= num_tokens)
      {
        self->tokens -= num_tokens;
        within_ratelimit = TRUE;
      }
    else
      {
        within_ratelimit = FALSE;
      }
  }
  g_mutex_unlock(self->lock);
  return within_ratelimit;
}

static gboolean
throttle_ratelimit_process_new_logs(ThrottleRateLimit *self, gint num_new_logs)
{
  throttle_ratelimit_add_new_tokens(self);
  return throttle_ratelimit_try_consume_tokens(self, num_new_logs);
}

static gboolean
filter_throttle_eval(FilterExprNode *s, LogMessage **msgs, gint num_msg, LogTemplateEvalOptions *options)
{
  FilterThrottle *self = (FilterThrottle *)s;

  const gchar *key;
  gssize len = 0;

  if (self->key_handle)
    {
      LogMessage *msg = msgs[num_msg-1];
      key = log_msg_get_value(msg, self->key_handle, &len);
      APPEND_ZERO(key, key, len);
    }
  else
    {
      key = "";
    }

  ThrottleRateLimit *rl;

  g_mutex_lock(self->map_lock);
  {
    rl = g_hash_table_lookup(self->rate_limits, key);

    if (!rl)
      {
        rl = throttle_ratelimit_new(self->rate);
        g_hash_table_insert(self->rate_limits, g_strdup(key), rl);
      }
  }
  g_mutex_unlock(self->map_lock);

  return throttle_ratelimit_process_new_logs(rl, num_msg);
}

static void
filter_throttle_free(FilterExprNode *s)
{
  FilterThrottle *self = (FilterThrottle *) s;

  g_hash_table_destroy(self->rate_limits);
  g_mutex_free(self->map_lock);
}

static gboolean
filter_throttle_init(FilterExprNode *s, GlobalConfig *cfg)
{
  FilterThrottle *self = (FilterThrottle *)s;

  if (self->rate <= 0)
    {
      msg_error("throttle: the rate() argument is required, and must be non zero in throttle filters");
      return FALSE;
    }

  return TRUE;
}

void
filter_throttle_set_key(FilterExprNode *s, NVHandle key_handle)
{
  FilterThrottle *self = (FilterThrottle *)s;
  self->key_handle = key_handle;
}

void
filter_throttle_set_rate(FilterExprNode *s, gint rate)
{
  FilterThrottle *self = (FilterThrottle *)s;
  self->rate = rate;
}

FilterExprNode *
filter_throttle_new(void)
{
  FilterThrottle *self = g_new0(FilterThrottle, 1);
  filter_expr_node_init_instance(&self->super);

  self->super.init = filter_throttle_init;
  self->super.eval = filter_throttle_eval;
  self->super.free_fn = filter_throttle_free;
  self->map_lock = g_mutex_new();
  self->rate_limits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)throttle_ratelimit_free);

  return &self->super;
}
