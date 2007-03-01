/*
 * Copyright (C) 1996-8 Michael R. Elkins <me@cs.hmc.edu>
 * 
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */ 

#include "mutt.h"
#include "mutt_curses.h"
#include "mx.h"
#include "mailbox.h"

#include <unistd.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* Minimal support for IMAP 4rev1 */

#define IMAP_PORT 143

#define SEQLEN 5

/* number of entries in the hash table */
#define IMAP_CACHE_LEN 10

enum
{
  IMAP_FATAL = 1,
  IMAP_NEW_MAIL,
  IMAP_BYE
};

typedef struct
{
  int index;
  char *path;
} IMAP_CACHE;

typedef struct
{
  short status;
  unsigned short sequence;
  unsigned short newMailCount;
  short xxx;
  IMAP_CACHE cache[IMAP_CACHE_LEN];
} IMAP_DATA;

static char ImapUser[SHORT_STRING] = { 0 };
static char ImapPass[SHORT_STRING] = { 0 };

static int imap_read_line (char *buf, size_t buflen, int fd)
{
  char ch;
  int i;

  for (i = 0; i < buflen; i++)
  {
    if (read (fd, &ch, 1) != 1)
      return (-1);
    if (ch == '\n')
      break;
    buf[i] = ch;
  }
  buf[i-1] = 0;
  return (i + 1);
}

static int imap_read_line_d (char *buf, size_t buflen, int fd)
{
  int r = imap_read_line (buf, buflen, fd);
  dprint (1,(debugfile,"imap_read_line_d():%s\n", buf));
  return r;
}

static void imap_make_sequence (char *buf, size_t buflen, CONTEXT *ctx)
{
  snprintf (buf, buflen, "a%04d", ((IMAP_DATA *) ctx->data)->sequence++);
}

static int imap_write (int fd, const char *buf)
{
  dprint (1,(debugfile,"imap_write():%s", buf));
  return (write (fd, buf, strlen (buf)));
}

static void imap_error (const char *where, const char *msg)
{
  dprint (1, (debugfile, "imap_error(): unexpected response in %s: %s\n", where, msg));
}

/* date is of the form: DD-MMM-YYYY HH:MM:SS +ZZzz */
static time_t imap_parse_date (char *s)
{
  struct tm t;
  time_t tz;

  t.tm_mday = (s[0] - '0') * 10 + (s[1] - '0');
  s += 2;
  if (*s != '-')
    return 0;
  s++;
  t.tm_mon = mutt_check_month (s);
  s += 3;
  if (*s != '-')
    return 0;
  s++;
  t.tm_year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0') - 1900;
  s += 4;
  if (*s != ' ')
    return 0;
  s++;

  /* time */
  t.tm_hour = (s[0] - '0') * 10 + (s[1] - '0');
  s += 2;
  if (*s != ':')
    return 0;
  s++;
  t.tm_min = (s[0] - '0') * 10 + (s[1] - '0');
  s += 2;
  if (*s != ':')
    return 0;
  s++;
  t.tm_sec = (s[0] - '0') * 10 + (s[1] - '0');
  s += 2;
  if (*s != ' ')
    return 0;
  s++;

  /* timezone */
  tz = ((s[1] - '0') * 10 + (s[2] - '0')) * 3600 +
    ((s[3] - '0') * 10 + (s[4] - '0')) * 60;
  if (s[0] == '+')
    tz = -tz;

  return (mutt_mktime (&t, 0) + tz);
}

static int imap_parse_fetch (HEADER *h, char *s)
{
  char tmp[SHORT_STRING];
  char *ptmp;
  int state = 0;

  if (!s)
    return (-1);

  h->read = 0;
  h->old = 0;

  while (*s)
  {
    SKIPWS (s);

    switch (state)
    {
      case 0:
	if (strncasecmp ("FLAGS", s, 5) == 0)
	{
	  s += 5;
	  SKIPWS (s);
	  if (*s != '(')
	  {
	    dprint (1, (debugfile, "imap_parse_fetch(): bogus FLAGS entry: %s\n", s));
	    return (-1); /* parse error */
	  }
	  s++;
	  state = 1;
	}
	else if (strncasecmp ("INTERNALDATE", s, 12) == 0)
	{
	  s += 12;
	  SKIPWS (s);
	  if (*s != '\"')
	  {
	    dprint (1, (debugfile, "imap_parse_fetch(): bogus INTERNALDATE entry: %s\n", s));
	    return (-1);
	  }
	  s++;
	  ptmp = tmp;
	  while (*s && *s != '\"')
	    *ptmp++ = *s++;
	  if (*s != '\"')
	    return (-1);
	  s++; /* skip past the trailing " */
	  *ptmp = 0;
	  h->received = imap_parse_date (tmp);
	}
	else if (strncasecmp ("RFC822.SIZE", s, 11) == 0)
	{
	  s += 11;
	  SKIPWS (s);
	  ptmp = tmp;
	  while (isdigit (*s))
	    *ptmp++ = *s++;
	  *ptmp = 0;
	}
	else if (*s == ')')
	  s++; /* end of request */
	else
	{
	  /* got something i don't understand */
	  imap_error ("imap_parse_fetch()", s);
	  return (-1);
	}
	break;
      case 1: /* flags */
	if (*s == ')')
	{
	  s++;
	  state = 0;
	}
	else if (strncasecmp ("\\deleted", s, 8) == 0)
	{
	  s += 8;
	  h->deleted = 1;
	}
	else if (strncasecmp ("\\flagged", s, 8) == 0)
	{
	  s += 8;
	  h->flagged = 1;
	}
	else if (strncasecmp ("\\answered", s, 9) == 0)
	{
	  s += 9;
	  h->replied = 1;
	}
	else if (strncasecmp ("\\seen", s, 5) == 0)
	{
	  s += 5;
	  h->read = 1;
	}
	else
	{
	  while (*s && !ISSPACE (*s) && *s != ')')
	    s++;
	}
	break;
    }
  }
  return 0;
}

static int imap_read_bytes (FILE *fp, int fd, long bytes)
{
  long pos;
  long len;
  char buf[LONG_STRING];

  for (pos = 0; pos < bytes; )
  {
    len = imap_read_line (buf, sizeof (buf), fd);
    if (len < 0)
      return (-1);
    pos += len;
    fputs (buf, fp);
    fputs ("\n", fp);
  }

  return 0;
}

/* returns 1 if the command result was OK, or 0 if NO or BAD */
static int imap_code (const char *s)
{
  s += SEQLEN;
  SKIPWS (s);
  return (strncasecmp ("OK", s, 2) == 0);
}

static char *imap_next_word (char *s)
{
  while (*s && !ISSPACE (*s))
    s++;
  SKIPWS (s);
  return s;
}

static int imap_handle_untagged (CONTEXT *ctx, char *s)
{
  char *pn;
  int count;

  s = imap_next_word (s);

  if (isdigit (*s))
  {
    pn = s;
    s = imap_next_word (s);

    if (strncasecmp ("EXISTS", s, 6) == 0)
    {
      /* new mail arrived */
      count = atoi (pn);

      if (count <= ctx->msgcount)
      {
	/* something is wrong because the server reported fewer messages
	 * than we previously saw
	 */
	mutt_error ("Fatal error.  Message count is out of sync!");
	((IMAP_DATA *) ctx->data)->status = IMAP_FATAL;
	mx_fastclose_mailbox (ctx);
	return (-1);
      }
      else
      {
	((IMAP_DATA *) ctx->data)->status = IMAP_NEW_MAIL;
	((IMAP_DATA *) ctx->data)->newMailCount = count;
      }
    }
  }
  else if (strncasecmp ("BYE", s, 3) == 0)
  {
    /* server shut down our connection */
    s += 3;
    SKIPWS (s);
    mutt_error (s);
    ((IMAP_DATA *) ctx->data)->status = IMAP_BYE;
    mx_fastclose_mailbox (ctx);
    return (-1);
  }
  else
  {
    dprint (1, (debugfile, "imap_unhandle_untagged(): unhandled request: %s\n",
		s));
  }

  return 0;
}

static int imap_read_header (CONTEXT *ctx, int msgno)
{
  char buf[LONG_STRING];
  FILE *fp;
  char tempfile[_POSIX_PATH_MAX];
  char seq[8];
  char *pc;
  char *pn;
  long bytes;

  ctx->hdrs[ctx->msgcount]->index = ctx->msgcount;

  mutt_mktemp (tempfile);
  if (!(fp = safe_fopen (tempfile, "w+")))
  {
    return (-1);
  }

  imap_make_sequence (seq, sizeof (seq), ctx);
  snprintf (buf, sizeof (buf), "%s FETCH %d RFC822.HEADER\r\n", seq, msgno + 1);
  imap_write (ctx->fd, buf);

  do
  {
    if (imap_read_line (buf, sizeof (buf), ctx->fd) < 0)
    {
      return (-1);
    }

    if (buf[0] == '*')
    {
      pc = buf;
      pc = imap_next_word (pc);
      pc = imap_next_word (pc);
      if (strncasecmp ("FETCH", pc, 5) == 0)
      {
	if (!(pc = strchr (pc, '{')))
	{
	  imap_error ("imap_read_header()", buf);
	  return (-1);
	}
	pc++;
	pn = pc;
	while (isdigit (*pc))
	  pc++;
	*pc = 0;
	bytes = atoi (pn);

	imap_read_bytes (fp, ctx->fd, bytes);
      }
      else if (imap_handle_untagged (ctx, buf) != 0)
	  return (-1);
    }
  }
  while (strncmp (seq, buf, SEQLEN) != 0);

  rewind (fp);
  ctx->hdrs[msgno]->env = mutt_read_rfc822_header (fp, ctx->hdrs[msgno]);

  fclose (fp);
  unlink (tempfile);

  /* get the status of this message */
  imap_make_sequence (seq, sizeof (seq), ctx);
  snprintf (buf, sizeof (buf), "%s FETCH %d FAST\r\n", seq, msgno + 1);
  imap_write (ctx->fd, buf);
  do
  {
    if (imap_read_line_d (buf, sizeof (buf), ctx->fd) < 0)
      break;
    if (buf[0] == '*')
    {
      pc = buf;
      pc = imap_next_word (pc);
      pc = imap_next_word (pc);
      if (strncasecmp ("FETCH", pc, 5) == 0)
      {
	if (!(pc = strchr (pc, '(')))
	{
	  imap_error ("imap_read_header()", buf);
	  return (-1);
	}
	if (imap_parse_fetch (ctx->hdrs[msgno], pc + 1) != 0)
	  return (-1);
      }
      else if (imap_handle_untagged (ctx, buf) != 0)
	return (-1);
    }
  }
  while (strncmp (seq, buf, SEQLEN) != 0)
    ;

  return 0;
}

static int imap_exec (char *buf, size_t buflen,
		      CONTEXT *ctx, const char *seq, const char *cmd)
{
  int count;

  imap_write (ctx->fd, cmd);

  do
  {
    if (imap_read_line_d (buf, buflen, ctx->fd) < 0)
      return (-1);

    if (buf[0] == '*' && imap_handle_untagged (ctx, buf) != 0)
      return (-1);
  }
  while (strncmp (buf, seq, SEQLEN) != 0);

  if (((IMAP_DATA *) ctx->data)->status == IMAP_NEW_MAIL)
  {
    /* read new mail messages */

    dprint (1, (debugfile, "imap_exec(): new mail detected\n"));
    mutt_message ("Fetching headers for new mail...");

    ((IMAP_DATA *) ctx->data)->status = 0;

    count = ((IMAP_DATA *) ctx->data)->newMailCount;
    while (count > ctx->hdrmax)
      mx_alloc_memory (ctx);

    while (ctx->msgcount < count)
    {
      ctx->hdrs[ctx->msgcount] = mutt_new_header ();
      imap_read_header (ctx, ctx->msgcount);
      mx_update_context (ctx); /* incremements ->msgcount */
      
      /* check to make sure that new mail hasn't arrived in the middle of
       * checking for new mail (sigh)
       */
      if (((IMAP_DATA *) ctx->data)->status == IMAP_NEW_MAIL)
      {
	count = ((IMAP_DATA *) ctx->data)->newMailCount;
	while (count > ctx->hdrmax)
	  mx_alloc_memory (ctx);
	((IMAP_DATA *) ctx->data)->status = 0;
      }
    }

    mutt_clear_error ();
  }

  if (!imap_code (buf))
  {
    char *pc;

    dprint (1, (debugfile, "imap_exec(): command failed: %s\n", buf));
    pc = buf + SEQLEN;
    SKIPWS (pc);
    pc = imap_next_word (pc);
    mutt_error (pc);
    sleep (1);
    return (-1);
  }

  return 0;
}

int imap_open_mailbox (CONTEXT *ctx)
{
  struct sockaddr_in sin;
  struct hostent *he;
  char buf[LONG_STRING];
  char bufout[LONG_STRING];
  char host[SHORT_STRING];
  char mailbox[_POSIX_PATH_MAX];
  char seq[16];
  int count = 0;
  int n;
  char *pc;

  pc = ctx->path;
  if (*pc != '{')
    return (-1);
  pc++;
  n = 0;
  while (*pc && *pc != '}')
    host[n++] = *pc++;
  host[n] = 0;
  if (!*pc)
    return (-1);
  pc++;
  strfcpy (mailbox, pc, sizeof (mailbox));

  if (!ImapUser[0])
    strfcpy (ImapUser, Username, sizeof (ImapUser));
  if (mutt_get_field ("IMAP Username: ", ImapUser, sizeof (ImapUser), 0) != 0 ||
      !ImapUser[0])
  {
    ImapUser[0] = 0;
    return (-1);
  }

  snprintf (buf, sizeof (buf), "Password for %s@%s: ", ImapUser, host);
  ImapPass[0] = 0;
  if (mutt_get_field (buf, ImapPass, sizeof (ImapPass), M_PASS) != 0 ||
      !ImapPass[0])
  {
    return (-1);
  }

  memset (&sin, 0, sizeof (sin));
  sin.sin_port = htons (IMAP_PORT);
  sin.sin_family = AF_INET;
  if ((he = gethostbyname (host)) == NULL)
  {
    mutt_perror (host);
    return (-1);
  }
  memcpy (&sin.sin_addr, he->h_addr_list[0], he->h_length);

  if ((ctx->fd = socket (AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
  {
    mutt_perror ("socket");
    return (-1);
  }

  mutt_message ("Connecting to %s...", host);

  if (connect (ctx->fd, (struct sockaddr *) &sin, sizeof (sin)) < 0)
  {
    mutt_perror ("connect");
    close (ctx->fd);
  }

  if (imap_read_line_d (buf, sizeof (buf), ctx->fd) < 0)
  {
    close (ctx->fd);
    return (-1);
  }

  if (strncmp ("* OK", buf, 4) != 0)
  {
    imap_error ("imap_open_mailbox()", buf);
    close (ctx->fd);
    return (-1);
  }

  /* create IMAP-specific state struct */
  ctx->data = safe_malloc (sizeof (IMAP_DATA));
  memset (ctx->data, 0, sizeof (IMAP_DATA));

  mutt_message ("Logging in...");
  imap_make_sequence (seq, sizeof (seq), ctx);
  snprintf (buf, sizeof (buf), "%s LOGIN %s %s\r\n", seq, ImapUser, ImapPass);
  if (imap_exec (buf, sizeof (buf), ctx, seq, buf) != 0)
  {
    imap_error ("imap_open_mailbox()", buf);
    return (-1);
  }

  mutt_message ("Selecting %s...", mailbox);
  imap_make_sequence (seq, sizeof (seq), ctx);
  snprintf (bufout, sizeof (bufout), "%s SELECT %s\r\n", seq, mailbox);
  imap_write (ctx->fd, bufout);

  do
  {
    if (imap_read_line_d (buf, sizeof (buf), ctx->fd) < 0)
      break;

    if (buf[0] == '*')
    {
      pc = buf + 2;

      if (isdigit (*pc))
      {
	char *pn = pc;

	while (*pc && isdigit (*pc))
	  pc++;
	*pc++ = 0;
	n = atoi (pn);
	SKIPWS (pc);
	if (strncasecmp ("EXISTS", pc, 6) == 0)
	  count = n;
      }
      else if (imap_handle_untagged (ctx, buf) != 0)
	return (-1);
    }
  }
  while (strncmp (seq, buf, strlen (seq)) != 0);

  mutt_message ("Fetching message headers...");
  ctx->hdrmax = count;
  ctx->hdrs = safe_malloc (count * sizeof (HEADER *));
  ctx->v2r = safe_malloc (count * sizeof (int));
  for (ctx->msgcount = 0; ctx->msgcount < count; )
  {
    ctx->hdrs[ctx->msgcount] = mutt_new_header ();

    /* `count' can get modified if new mail arrives while fetching the
     * header for this message
     */
    if (imap_read_header (ctx, ctx->msgcount) != 0)
    {
      mx_fastclose_mailbox (ctx);
      return (-1);
    }
    mx_update_context (ctx); /* increments ->msgcount */

    /* in case we get new mail while fetching the headers */
    if (((IMAP_DATA *) ctx->data)->status == IMAP_NEW_MAIL)
    {
      count = ((IMAP_DATA *) ctx->data)->newMailCount;
      while (count > ctx->hdrmax)
	mx_alloc_memory (ctx);
      ((IMAP_DATA *) ctx->data)->status = 0;
    }
  }

  return 0;
}

int imap_fetch_message (MESSAGE *msg, CONTEXT *ctx, int msgno)
{
  char seq[8];
  char buf[LONG_STRING];
  char path[_POSIX_PATH_MAX];
  char *pc;
  char *pn;
  long bytes;
  IMAP_CACHE *cache;

  /* see if we already have the message in our cache */
  cache = &((IMAP_DATA *) ctx->data)->cache[ctx->hdrs[msgno]->index % IMAP_CACHE_LEN];

  if (cache->path)
  {
    if (cache->index == ctx->hdrs[msgno]->index)
    {
      /* yes, so just return a pointer to the message */
      if (!(msg->fp = fopen (cache->path, "r")))
      {
	mutt_perror (cache->path);
	return (-1);
      }
      return 0;
    }
    else
    {
      /* clear the previous entry */
      unlink (cache->path);
      free (cache->path);
    }
  }

  mutt_message ("Fetching message...");

  cache->index = ctx->hdrs[msgno]->index;
  mutt_mktemp (path);
  cache->path = safe_strdup (path);
  if (!(msg->fp = safe_fopen (path, "w+")))
  {
    safe_free ((void **) &cache->path);
    return (-1);
  }

  imap_make_sequence (seq, sizeof (seq), ctx);
  snprintf (buf, sizeof (buf), "%s FETCH %d RFC822\r\n", seq,
	    ctx->hdrs[msgno]->index + 1);
  imap_write (ctx->fd, buf);
  do
  {
    if (imap_read_line (buf, sizeof (buf), ctx->fd) < 0)
    {
      return (-1);
    }

    if (buf[0] == '*')
    {
      pc = buf;
      pc = imap_next_word (pc);
      pc = imap_next_word (pc);
      if (strncasecmp ("FETCH", pc, 5) == 0)
      {
	if (!(pc = strchr (buf, '{')))
	{
	  imap_error ("imap_fetch_message()", buf);
	  return (-1);
	}
	pc++;
	pn = pc;
	while (isdigit (*pc))
	  pc++;
	*pc = 0;
	bytes = atoi (pn);
	imap_read_bytes (msg->fp, ctx->fd, bytes);
      }
      else if (imap_handle_untagged (ctx, buf) != 0)
	return (-1);
    }
  }
  while (strncmp (buf, seq, SEQLEN) != 0)
    ;

  if (!imap_code (buf))
  {
    return (-1);
  }

  return 0;
}

int imap_close_connection (CONTEXT *ctx)
{
  char buf[LONG_STRING];
  char seq[8];

  /* if the server didn't shut down on us, close the connection gracefully */
  if (((IMAP_DATA *) ctx->data)->status != IMAP_BYE)
  {
    mutt_message ("Closing connection to IMAP server...");
    imap_make_sequence (seq, sizeof (seq), ctx);
    snprintf (buf, sizeof (buf), "%s LOGOUT\r\n", seq);
    imap_write (ctx->fd, buf);
    do
    {
      if (imap_read_line_d (buf, sizeof (buf), ctx->fd) < 0)
	break;
    }
    while (strncmp (seq, buf, SEQLEN) != 0);
    mutt_clear_error ();
  }
  close (ctx->fd);
  return 0;
}

static int make_delete_list (char *list, size_t listlen, CONTEXT *ctx)
{
  int first = -1, last = -1;
  int n;
  char tmp[LONG_STRING];

  *list = 0;
  for (n=0; n<ctx->msgcount; n++)
  {
    if (ctx->hdrs[n]->deleted)
    {
      if (first < 0)
      {
	first = n;
	last = n;
      }
      else if (last != n - 1)
      {
	if (first != last)
	  snprintf (tmp, sizeof (tmp), "%d:%d", first + 1, last + 1);
	else
	  snprintf (tmp, sizeof (tmp), "%d", first + 1);
	if (list[0])
	  strcat (list, ",");
	strcat (list, tmp);
	first = last = n;
      }
      else
	last = n;
    }
  }

  if (first >= 0)
  {
    if (first != last)
      snprintf (tmp, sizeof (tmp), "%d:%d", first + 1, last + 1);
    else
      snprintf (tmp, sizeof (tmp), "%d", first + 1);
    if (list[0])
      strcat (list, ",");
    strcat (list, tmp);
  }

  return 0;
}

int imap_sync_mailbox (CONTEXT *ctx)
{
  char seq[8];
  char buf[LONG_STRING];
  char tmp[LONG_STRING];
  int n;

  /* save status changes */
  mutt_message ("Saving message status flags...");
  for (n = 0; n < ctx->msgcount; n++)
  {
    if (!ctx->hdrs[n]->deleted && ctx->hdrs[n]->changed)
    {
      if (ctx->hdrs[n]->read)
	strcat (tmp, "\\Seen ");
      if (ctx->hdrs[n]->flagged)
	strcat (tmp, "\\Flagged ");
      if (ctx->hdrs[n]->replied)
	strcat (tmp, "\\Answered");
      mutt_remove_trailing_ws (tmp);

      imap_make_sequence (seq, sizeof (seq), ctx);
      snprintf (buf, sizeof (buf), "%s STORE %d FLAGS.SILENT (%s)\r\n", seq, tmp);
      if (imap_exec (buf, sizeof (buf), ctx, seq, buf) != 0)
      {
	imap_error ("imap_sync_mailbox()", buf);
	return (-1);
      }
    }
  }

  mutt_message ("Marking messages as deleted...");
  make_delete_list (tmp, sizeof (tmp), ctx);
  imap_make_sequence (seq, sizeof (seq), ctx);
  snprintf (buf, sizeof (buf), "%s STORE %s +FLAGS.SILENT (\\Deleted)\r\n", seq, tmp);
  if (imap_exec (buf, sizeof (buf), ctx, seq, buf) != 0)
  {
    imap_error ("imap_sync_mailbox()", buf);
    return (-1);
  }

  return 0;
}

void imap_fastclose_mailbox (CONTEXT *ctx)
{
  int i;

  imap_close_connection (ctx);
  for (i = 0; i < IMAP_CACHE_LEN; i++)
  {
    if (((IMAP_DATA *) ctx->data)->cache[i].path)
    {
      unlink (((IMAP_DATA *) ctx->data)->cache[i].path);
      safe_free ((void **) &((IMAP_DATA *) ctx->data)->cache[i].path);
    }
  }
  safe_free ((void **) &ctx->data);
}

/* commit changes and terminate connection */
int imap_close_mailbox (CONTEXT *ctx)
{
  char seq[8];
  char buf[LONG_STRING];

  /* tell the server to commit changes */
  mutt_message ("Closing mailbox...");
  imap_make_sequence (seq, sizeof (seq), ctx);
  snprintf (buf, sizeof (buf), "%s CLOSE\r\n", seq);
  if (imap_exec (buf, sizeof (buf), ctx, seq, buf) != 0)
  {
    imap_error ("imap_close_mailbox()", buf);
    return (-1);
  }
  return 0;
}

/* use the NOOP command to poll for new mail */
int imap_check_mailbox (CONTEXT *ctx, int *index_hint)
{
  char seq[8];
  char buf[LONG_STRING];
  int msgcount = ctx->msgcount;

  imap_make_sequence (seq, sizeof (seq), ctx);
  snprintf (buf, sizeof (buf), "%s NOOP\r\n", seq);
  if (imap_exec (buf, sizeof (buf), ctx, seq, buf) != 0)
  {
    imap_error ("imap_check_mailbox()", buf);
    return (-1);
  }

  return (msgcount != ctx->msgcount);
}