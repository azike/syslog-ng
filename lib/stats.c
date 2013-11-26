/*
 * Copyright (c) 2002-2013 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2012 Balázs Scheidler
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
  
#include "stats.h"
#include "stats-syslog.h"
#include "stats-log.h"
#include "stats-registry.h"
#include "messages.h"
#include "timeutils.h"
#include "misc.h"

#include <string.h>
#include <iv.h>

/*
 * The statistics module
 *
 * Various components of syslog-ng require counters to keep track of various
 * metrics, such as number of messages processed, dropped or stored in a
 * queue. For this purpose, this module provides an easy to use API to
 * register and keep track these counters, and also to publish them to
 * external programs via a UNIX domain socket.
 *
 * Each counter has the following properties:
 *   * source component: enumerable type, that specifies the syslog-ng
 *     component that the given counter belongs to, examples:
 *       source.file, destination.file, center, source.socket, etc.
 *
 *   * id: the unique identifier of the syslog-ng configuration item that
 *     this counter belongs to. Named configuration elements (source,
 *     destination, etc) use their "name" here. Other components without a
 *     name use either an autogenerated ID (that can change when the
 *     configuration file changes), or an explicit ID configured by the
 *     administrator.
 * 
 *   * instance: each configuration element may track several sets of
 *     counters. This field specifies an identifier that makes a group of
 *     counters unique. For instance:
 *      - source TCP drivers use the IP address of the client here
 *      - destination file writers use the expanded filename
 *      - for those which have no notion for instance, NULL is used
 *
 *   * state: dynamic, active or orphaned, this indicates whether the given
 *     counter is in use or in orphaned state
 *
 *   * type: counter type (processed, dropped, stored, etc)
 *
 * Threading
 *
 * Once registered, changing the counters is thread safe (but see the
 * note on set/get), inc/dec is generally safe. To register counters,
 * the stats code must run in the main thread (assuming init/deinit is
 * running) or the stats lock must be acquired using stats_lock() and
 * stats_unlock(). This API is used to allow batching multiple stats
 * operations under the protection of the same lock acquiral.
 */

StatsOptions *stats_options;

gboolean
stats_check_level(gint level)
{
  if (stats_options)
    return (stats_options->level >= level);
  else
    return level == 0;
}

static const gchar *
stats_get_direction_name(gint source)
{
  return (source & SCS_SOURCE ? "src." : (source & SCS_DESTINATION ? "dst." : ""));
}

static const gchar *
stats_get_source_name(gint source)
{
  static const gchar *source_names[SCS_MAX] =
  {
    "none",
    "file",
    "pipe",
    "tcp",
    "udp",
    "tcp6",
    "udp6",
    "unix-stream",
    "unix-dgram",
    "syslog",
    "network",
    "internal",
    "logstore",
    "program",
    "sql",
    "sun-streams",
    "usertty",
    "group",
    "center",
    "host",
    "global",
    "mongodb",
    "class",
    "rule_id",
    "tag",
    "severity",
    "facility",
    "sender",
    "smtp",
    "amqp",
    "stomp",
    "redis",
    "snmp",
  };
  return source_names[source & SCS_SOURCE_MASK];
}

const gchar *
stats_get_type_name(gint type)
{
  static const gchar *tag_names[SC_TYPE_MAX] =
  {
    /* [SC_TYPE_DROPPED]   = */ "dropped",
    /* [SC_TYPE_PROCESSED] = */ "processed",
    /* [SC_TYPE_STORED]   = */  "stored",
    /* [SC_TYPE_SUPPRESSED] = */ "suppressed",
    /* [SC_TYPE_STAMP] = */ "stamp",
  };

  return tag_names[type];
}

/* buf is a scratch area which is not always used, the return value is
 * either a locally managed string or points to @buf.  */
const gchar *
stats_get_direction_and_source_name(gint source, gchar *buf, gsize buf_len)
{
  if ((source & SCS_SOURCE_MASK) == SCS_GROUP)
    {
      if (source & SCS_SOURCE)
        return "source";
      else if (source & SCS_DESTINATION)
        return "destination";
      else
        g_assert_not_reached();
    }
  else
    {
      g_snprintf(buf, buf_len, "%s%s", stats_get_direction_name(source), stats_get_source_name(source));
      return buf;
    }
}


static gboolean
stats_counter_is_expired(StatsCluster *sc, time_t now)
{
  time_t tstamp;

  /* check if dynamic entry, non-dynamic entries cannot be too large in
   * numbers, those are never pruned */
  if (!sc->dynamic)
    return FALSE;

  /* this entry is being updated, cannot be too old */    
  if (sc->ref_cnt > 0)
    return FALSE;

  /* check if timestamp is stored, no timestamp means we can't expire it.
   * All dynamic entries should have a timestamp.  */
  if ((sc->live_mask & (1 << SC_TYPE_STAMP)) == 0)
    return FALSE;

  tstamp = sc->counters[SC_TYPE_STAMP].value;
  return (tstamp <= now - stats_options->lifetime);
}

typedef struct _StatsTimerState
{
  GTimeVal now;
  time_t oldest_counter;
  gint dropped_counters;
  EVTREC *stats_event;
} StatsTimerState;

static gboolean
stats_prune_counter(StatsCluster *sc, StatsTimerState *st)
{
  gboolean expired;

  expired = stats_counter_is_expired(sc, st->now.tv_sec);
  if (expired)
    {
      time_t tstamp = sc->counters[SC_TYPE_STAMP].value;
      if ((st->oldest_counter) == 0 || st->oldest_counter > tstamp)
        st->oldest_counter = tstamp;
      st->dropped_counters++;
    }
  return expired;
}

static gboolean
stats_format_and_prune_cluster(StatsCluster *sc, gpointer user_data)
{
  StatsTimerState *st = (StatsTimerState *) user_data;

  stats_log_format_cluster(sc, st->stats_event);
  return stats_prune_counter(sc, st);
}

void
stats_publish_and_prune_counters(void)
{
  StatsTimerState st;
  gboolean publish = (stats_options->log_freq > 0);
  
  st.oldest_counter = 0;
  st.dropped_counters = 0;
  st.stats_event = NULL;
  cached_g_current_time(&st.now);

  if (publish)
    st.stats_event = msg_event_create(EVT_PRI_INFO, "Log statistics", NULL);

  stats_lock();
  stats_foreach_cluster_remove(stats_format_and_prune_cluster, &st);
  stats_unlock();

  if (publish)
    msg_event_send(st.stats_event);

  if (st.dropped_counters > 0)
    {
      msg_notice("Pruning stats-counters have finished",
                 evt_tag_int("dropped", st.dropped_counters),
                 evt_tag_long("oldest-timestamp", (long) st.oldest_counter),
                 NULL);
    }
}


static void
stats_timer_rearm(struct iv_timer *timer)
{
  gint freq = GPOINTER_TO_INT(timer->cookie);
  if (freq > 0)
    {
      /* arm the timer */
      iv_validate_now();
      timer->expires = iv_now;
      timespec_add_msec(&timer->expires, freq * 1000);
      iv_timer_register(timer);
    }
}

static void
stats_timer_init(struct iv_timer *timer, void (*handler)(void *), gint freq)
{
  IV_TIMER_INIT(timer);
  timer->handler = handler;
  timer->cookie = GINT_TO_POINTER(freq);
}

static void
stats_timer_kill(struct iv_timer *timer)
{
  if (!timer->handler)
    return;
  if (iv_timer_registered(timer))
    iv_timer_unregister(timer);
}

static struct iv_timer stats_timer;


static void
stats_timer_elapsed(gpointer st)
{
  stats_publish_and_prune_counters();
  stats_timer_rearm(&stats_timer);
}

void
stats_timer_reinit(void)
{
  gint freq;

  freq = stats_options->log_freq;
  if (!freq)
    freq = stats_options->lifetime <= 1 ? 1 : stats_options->lifetime / 2;

  stats_timer_kill(&stats_timer);
  stats_timer_init(&stats_timer, stats_timer_elapsed, freq);
  stats_timer_rearm(&stats_timer);
}

void
stats_reinit(StatsOptions *options)
{
  stats_options = options;
  stats_syslog_reinit();
  stats_timer_reinit();
}

void
stats_init(void)
{
  stats_registry_init();
}

void
stats_destroy(void)
{
  stats_registry_deinit();
}

void
stats_options_defaults(StatsOptions *options)
{
  options->level = 0;
  options->log_freq = 600;
  options->lifetime = 600;
}
