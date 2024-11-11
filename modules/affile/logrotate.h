
#ifndef LOGROTATE_H_INCLUDED
#define LOGROTATE_H_INCLUDED

#include "syslog-ng.h"
#include <logproto/logproto.h>
#include "messages.h"

typedef enum
{
  DAILY, WEEKLY, MONTHLY, YEARLY, NONE
} interval_t;

typedef enum
{
  LR_SUCCESS, LR_ERROR
} LogRotateStatus;

typedef struct _LogRotateOptions
{
  gboolean enable;
  gsize size;
  gsize max_rotations;
  interval_t interval;
  const gchar *date_format;
} LogRotateOptions;

gboolean is_logrotate_enabled(LogRotateOptions *logrotate_options);
gboolean is_logrotate_pending(LogRotateOptions *logrotate_options, const gchar *filename);
void logrotate_options_defaults(LogRotateOptions *logrotate_options);
LogRotateStatus do_logrotate(LogRotateOptions *logrotate_options, const gchar *filename);

#endif
