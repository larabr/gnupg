/* command.c - SCdaemon command handler
 *	Copyright (C) 2001, 2002 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "scdaemon.h"
#include "../assuan/assuan.h"

#define set_error(e,t) assuan_set_error (ctx, ASSUAN_ ## e, (t))

/* Data used to associate an Assuan context with local server data */
struct server_local_s {
  ASSUAN_CONTEXT assuan_ctx;
};


/* Check whether the option NAME appears in LINE */
static int
has_option (const char *line, const char *name)
{
  const char *s;
  int n = strlen (name);

  s = strstr (line, name);
  return (s && (s == line || spacep (s-1)) && (!s[n] || spacep (s+n)));
}




/* Note, that this reset_notify is also used for cleanup purposes. */
static void
reset_notify (ASSUAN_CONTEXT ctx)
{
  CTRL ctrl = assuan_get_pointer (ctx); 

  if (ctrl->card_ctx)
    {
      card_close (ctrl->card_ctx);
      ctrl->card_ctx = NULL;
    }
}


static int
option_handler (ASSUAN_CONTEXT ctx, const char *key, const char *value)
{
  return 0;
}


/* LEARN [--force]

   Learn all useful information of the currently inserted card.  When
   used without the force options, the command might do an INQUIRE
   like this:

      INQUIRE KNOWNCARDP <hexstring_with_serialNumber> <timestamp>

   The client should just send an "END" if the processing should go on
   or a "CANCEL" to force the function to terminate with a Cancel
   error message.  The response of this command is a list of status
   lines formatted as this:

     S KEYPAIRINFO <hexstring_with_keygrip> <hexstring_with_id>

   If there is no certificate yet stored on the card a single "X" is
   returned as the keygrip.

*/
static int
cmd_learn (ASSUAN_CONTEXT ctx, char *line)
{
  CTRL ctrl = assuan_get_pointer (ctx);
  int rc = 0;
  int idx;

  /* if this is the first command issued for a new card, open the card and 
     and create a context */
  if (!ctrl->card_ctx)
    {
      rc = card_open (&ctrl->card_ctx);
      if (rc)
        return map_to_assuan_status (rc);
    }

  /* Unless the force option is used we try a shortcut by identifying
     the card using a serial number and inquiring the client with
     that. The client may choose to cancel the operation if he already
     knows about this card */
  {
    char *serial_and_stamp;
    char *serial;
    time_t stamp;
   
    rc = card_get_serial_and_stamp (ctrl->card_ctx, &serial, &stamp);
    if (rc)
      return map_to_assuan_status (rc);
    rc = asprintf (&serial_and_stamp, "%s %lu", serial, (unsigned long)stamp);
    xfree (serial);
    if (rc < 0)
      return ASSUAN_Out_Of_Core;
    rc = 0;
    assuan_write_status (ctx, "SERIALNO", serial_and_stamp);

    if (!has_option (line, "--force"))
      {
        char *command;

        rc = asprintf (&command, "KNOWNCARDP %s", serial_and_stamp);
        if (rc < 0)
          {
            free (serial_and_stamp);
            return ASSUAN_Out_Of_Core;
          }
        rc = 0;
        rc = assuan_inquire (ctx, command, NULL, NULL, 0); 
        free (command);  /* (must use standard free here) */
        if (rc)
          {
            if (rc != ASSUAN_Canceled)
              log_error ("inquire KNOWNCARDP failed: %s\n",
                         assuan_strerror (rc));
            free (serial_and_stamp);
            return rc; 
          }
        /* not canceled, so we have to proceeed */
      }
    free (serial_and_stamp);
  }

  for (idx=0; !rc; idx++)
    {
      unsigned char keygrip[20];
      unsigned char *keyid;
      size_t nkeyid;
      int no_cert = 0;

      rc = card_enum_keypairs (ctrl->card_ctx, idx, 
                               keygrip, &keyid, &nkeyid);
      if (rc == GNUPG_Missing_Certificate && keyid)
        {
          /* this does happen with an incomplete personalized
             card; i.e. during the time we have stored the key on the
             card but not stored the certificate; probably becuase it
             has not yet been received back from the CA.  Note that we
             must release KEYID in this case. */
          rc = 0; 
          no_cert = 1;
        }
      if (!rc)
        {
          char *buf, *p;

          buf = p = xtrymalloc (40+1+9+2*nkeyid+1);
          if (!buf)
            rc = GNUPG_Out_Of_Core;
          else
            {
              int i;
              
              if (no_cert)
                *p++ = 'X';
              else
                {
                  for (i=0; i < 20; i++, p += 2)
                    sprintf (p, "%02X", keygrip[i]);
                }
              *p++ = ' ';
              /* fixme: we need to get the pkcs-15 DF from the card function */
              p = stpcpy (p, "3F005015.");
              for (i=0; i < nkeyid; i++, p += 2)
                sprintf (p, "%02X", keyid[i]);
              *p = 0;
              assuan_write_status (ctx, "KEYPAIRINFO", buf);
              xfree (buf);
            }
        }
      xfree (keyid);
    }
  if (rc == -1)
    rc = 0;


  return map_to_assuan_status (rc);
}



/* READCERT <hexified_certid>

 */
static int
cmd_readcert (ASSUAN_CONTEXT ctx, char *line)
{
  CTRL ctrl = assuan_get_pointer (ctx);
  int rc;
  unsigned char *cert;
  size_t ncert;

  if (!ctrl->card_ctx)
    {
      rc = card_open (&ctrl->card_ctx);
      if (rc)
        return map_to_assuan_status (rc);
    }

  rc = card_read_cert (ctrl->card_ctx, line, &cert, &ncert);
  if (rc)
    {
      log_error ("card_read_cert failed: %s\n", gnupg_strerror (rc));
    }
  if (!rc)
    {
      rc = assuan_send_data (ctx, cert, ncert);
      xfree (cert);
      if (rc)
        return rc;
    }

  return map_to_assuan_status (rc);
}


/* Tell the assuan library about our commands */
static int
register_commands (ASSUAN_CONTEXT ctx)
{
  static struct {
    const char *name;
    int cmd_id;
    int (*handler)(ASSUAN_CONTEXT, char *line);
  } table[] = {
    { "LEARN", 0, cmd_learn },
    { "READCERT", 0, cmd_readcert },
    { "",     ASSUAN_CMD_INPUT, NULL }, 
    { "",     ASSUAN_CMD_OUTPUT, NULL }, 
    { NULL }
  };
  int i, j, rc;

  for (i=j=0; table[i].name; i++)
    {
      rc = assuan_register_command (ctx,
                                    table[i].cmd_id? table[i].cmd_id
                                                   : (ASSUAN_CMD_USER + j++),
                                    table[i].name, table[i].handler);
      if (rc)
        return rc;
    } 
  assuan_set_hello_line (ctx, "GNU Privacy Guard's Smartcard server ready");

  assuan_register_reset_notify (ctx, reset_notify);
  assuan_register_option_handler (ctx, option_handler);
  return 0;
}


/* Startup the server.  If LISTEN_FD is given as -1, this is simple
   piper server, otherwise it is a regular server */
void
scd_command_handler (int listen_fd)
{
  int rc;
  ASSUAN_CONTEXT ctx;
  struct server_control_s ctrl;

  memset (&ctrl, 0, sizeof ctrl);
  scd_init_default_ctrl (&ctrl);
  
  if (listen_fd == -1)
    {
      int filedes[2];

      filedes[0] = 0;
      filedes[1] = 1;
      rc = assuan_init_pipe_server (&ctx, filedes);
    }
  else
    {
      rc = assuan_init_socket_server (&ctx, listen_fd);
    }
  if (rc)
    {
      log_error ("failed to initialize the server: %s\n",
                 assuan_strerror(rc));
      scd_exit (2);
    }
  rc = register_commands (ctx);
  if (rc)
    {
      log_error ("failed to register commands with Assuan: %s\n",
                 assuan_strerror(rc));
      scd_exit (2);
    }
  assuan_set_pointer (ctx, &ctrl);
  ctrl.server_local = xcalloc (1, sizeof *ctrl.server_local);
  ctrl.server_local->assuan_ctx = ctx;

  if (DBG_ASSUAN)
    assuan_set_log_stream (ctx, log_get_stream ());

  for (;;)
    {
      rc = assuan_accept (ctx);
      if (rc == -1)
        {
          break;
        }
      else if (rc)
        {
          log_info ("Assuan accept problem: %s\n", assuan_strerror (rc));
          break;
        }
      
      rc = assuan_process (ctx);
      if (rc)
        {
          log_info ("Assuan processing failed: %s\n", assuan_strerror (rc));
          continue;
        }
    }
  reset_notify (ctx); /* used for cleanup */

  assuan_deinit_server (ctx);
}
