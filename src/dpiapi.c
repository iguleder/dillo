/*
 * File: dpiapi.c
 *
 * Copyright (C) 2004 Jorge Arellano Cid <jcid@dillo.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* Support for dpi/dpip from Dillo's side */

#include "msg.h"
#include "browser.h"
#include "capi.h"
#include "interface.h"
#include "dpiapi.h"  /* for prototypes */
#include "../dpip/dpip.h"


/*----------------------------------------------------------------------------
 * Dialog interface
 */

/* This variable can be eliminated as a parameter with a cleaner API. */
static char *dialog_server = NULL;


/*
 * Generic callback function for dpip dialogs.
 */
static void Dpiapi_dialog_answer_cb(BrowserWindow *bw)
{
   DialogAnswer *answer = bw->question_dialog_answer;
   char *cmd, numstr[16];

   /* make dpip tag with the answer */
   g_snprintf(numstr, 16, "%d", answer->alt_num);
   cmd = a_Dpip_build_cmd("cmd=%s to_cmd=%s msg=%s",
                          "answer", "dialog", numstr);

   /* Send answer */
   a_Capi_dpi_send_cmd(NULL, bw, cmd, dialog_server, 0);

   /* cleanup */
   bw->question_dialog_data = NULL;
   g_free(answer->this);
   bw->question_dialog_answer = NULL;
}

/*
 * Process a dpip "dialog" command from any dpi.
 */
void a_Dpiapi_dialog(BrowserWindow *bw, char *server, char *dpip_tag)
{
   char *question, *alt1, *alt2, *alt3, *alt4, *alt5;
   size_t dpip_tag_len;

   MSG("a_Dpiapi_dialog:\n");
   MSG("  dpip_tag: %s\n", dpip_tag);

   /* set the module scoped variable */
   dialog_server = server;

   /* other options can be parsed the same way */
   dpip_tag_len = strlen(dpip_tag);
   question = a_Dpip_get_attr(dpip_tag, dpip_tag_len, "msg");
   alt1 = a_Dpip_get_attr(dpip_tag, dpip_tag_len, "alt1");
   alt2 = a_Dpip_get_attr(dpip_tag, dpip_tag_len, "alt2");
   alt3 = a_Dpip_get_attr(dpip_tag, dpip_tag_len, "alt3");
   alt4 = a_Dpip_get_attr(dpip_tag, dpip_tag_len, "alt4");
   alt5 = a_Dpip_get_attr(dpip_tag, dpip_tag_len, "alt5");

   a_Interface_question_dialog(
      bw, question, TRUE,
      alt1, alt2, alt3, alt4, alt5,
      (GtkSignalFunc) Dpiapi_dialog_answer_cb);

   g_free(alt1); g_free(alt2); g_free(alt3); g_free(alt4); g_free(alt5);
   g_free(question);
}

