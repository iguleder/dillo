/*
 * Dpi for HTTPS.
 *
 *
 *
 *                            W A R N I N G
 *
 * One of the important things to have in mind is about whether
 * unix domain sockets (UDS) are secure for https. I mean, root can always
 * snoop on sockets (regardless of permissions) so he'd be able to "see" all
 * the traffic. OTOH, if someone has root access on a machine he can do
 * anything, and that includes modifying the binaries, peeking-up in
 * memory space, installing a key-grabber, ...
 *
 *
 * Copyright 2003, 2004 Jorge Arellano Cid <jcid@dillo.org>
 * Copyright 2004 Garrett Kajmowicz <gkajmowi@tbaytel.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * As a special exception permission is granted to link the code of
 * the https dillo plugin with the OpenSSL project's "OpenSSL"
 * library, and distribute the linked executables, without including
 * the source code for OpenSSL in the source distribution. You must
 * obey the GNU General Public License, version 2, in all respects
 * for all of the code used other than "OpenSSL".
 *
 */

/*
 * TODO: a lot of things, this is just a bare bones example.
 *
 * For instance:
 * - Handle cookies (now that they arrive with the dpip tag, it needs
 *   testing).
 * - Certificate authentication (asking the user in case it can't be verified)
 * - Certificate management.
 * - Session caching ...
 *
 */

#include <config.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <glib.h>
#include "../dpip/dpip.h"
#include "dpiutil.h"

#ifdef ENABLE_SSL

#	ifdef ENABLE_AXTLS
#		include <axTLS/ssl.h>
#	else
#		include <openssl/ssl.h>
#		include <openssl/rand.h>
#	endif

static int get_network_connection(gchar * url);
static int handle_certificate_problem(SSL * ssl_connection);
#	ifndef ENABLE_AXTLS
static int save_certificate_home(X509 * cert);
#	endif

#endif

int main(void);

/*---------------------------------------------------------------------------*/
/*
 * Global variables
 */
static gchar *root_url = NULL;  /*Holds the URL we are connecting to*/
static SockHandler *sh;


#ifdef ENABLE_SSL

/*
 * Read the answer dpip tag for a dialog and return the number for
 * the user-selected alternative.
 * Return: (-1: parse error, 0: window closed, 1-5 alt. number)
 */
static gint dialog_get_answer_number(void)
{
   gint response_number = -1;
   gchar *dpip_tag, *response;

   /* Read the dpi command from STDIN */
   dpip_tag = sock_handler_read(sh);
   response = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "msg");
   response_number = (response) ? strtol (response, NULL, 10) : -1;
   g_free(dpip_tag);
   g_free(response);

   return response_number;
}


/*
 *  This function does all of the work with SSL
 */
static void yes_ssl_support(void)
{
   /* The following variable will be set to 1 in the event of
    * an error and skip any further processing
    */
   gint exit_error = 0;
   SSL_CTX * ssl_context = NULL;
   SSL * ssl_connection = NULL;

   gchar *dpip_tag = NULL, *cmd = NULL, *url = NULL, *http_query = NULL;
#ifdef ENABLE_AXTLS
   uint8_t *buf = NULL;
#else
   gchar buf[4096];
#endif
   int retval = 0;
   int network_socket = -1;
#ifdef ENABLE_AXTLS
   int status = 0;
#endif


   g_printerr("{In https.filter.dpi}\n");

   /*Initialize library*/
#ifdef ENABLE_AXTLS
   ssl_context = ssl_ctx_new(0, 0);
   if (NULL == ssl_context) {
      exit_error = 1;
   }

   if (exit_error == 0){
      ssl_obj_load(ssl_context,
                   SSL_OBJ_X509_CACERT,
                   "/etc/ssl/certs/ca-certificates.crt",
                   NULL);
   }

#else
   SSL_load_error_strings();
   SSL_library_init();
   if (RAND_status() != 1){
      /*Insufficient entropy.  Deal with it?*/
      g_printerr("Insufficient random entropy\n");
   }

   /*Create context and SSL object*/
   if (exit_error == 0){
      ssl_context = SSL_CTX_new(SSLv23_client_method());
      if (ssl_context == NULL){
         g_printerr("Error creating SSL context\n");
         exit_error = 1;
      }
   }

   /*Set directory to load certificates from*/
   if (exit_error == 0){
      if (SSL_CTX_set_default_verify_paths(ssl_context) == 0) {
         g_printerr("Error opening system x509 certificate location\n");
         exit_error = 1;
      }
   }

   if (exit_error == 0){
      g_snprintf(buf, 4095, "%s/.dillo/certs/", g_get_home_dir() );
      if (SSL_CTX_load_verify_locations(ssl_context, NULL, buf )==0){
         g_printerr("Error opening user x509 certificate location\n");
         exit_error = 1;
      }
   }

   if (exit_error == 0){
      ssl_connection = SSL_new(ssl_context);
      if (ssl_connection == NULL){
         g_printerr("Error creating SSL connection\n");
         exit_error = 1;
      }
   }
#endif

   if (exit_error == 0){
#ifndef ENABLE_AXTLS
      /* Need to do the following if we want to deal with all
       * possible ciphers
       */
      SSL_set_cipher_list(ssl_connection, "ALL");

      /* Need to do this if we want to have the option of dealing
       * with self-signed certs
       */
      SSL_set_verify(ssl_connection, SSL_VERIFY_NONE, 0);
#endif

      /*Get the network address and command to be used*/
      dpip_tag = sock_handler_read(sh);
      cmd = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "cmd");
      url = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "url");
      http_query = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "query");

      if (cmd == NULL || url == NULL || http_query == NULL){
         g_printerr("***Value of cmd, url or http_query is NULL"
                    " - cannot continue\n");
         exit_error = 1;
      }
   }

   if (exit_error == 0){
      network_socket = get_network_connection(url);
      if (network_socket<0){
         g_printerr("Network socket create error\n");
         exit_error = 1;
      }
   }

#ifdef ENABLE_AXTLS
   if (exit_error == 0) {
      ssl_connection = ssl_client_new(ssl_context, network_socket, NULL, 32);
      if (NULL == ssl_connection) {
         exit_error = 1;
      }
   }

   if (exit_error == 0) {
      status = ssl_handshake_status(ssl_connection);
      if (SSL_OK != status) {
         g_printerr("SSL handshake failed\n");
         exit_error = 1;
      }
   }
#else
   if (exit_error == 0){
      /* Configure SSL to use network file descriptor */
      if (SSL_set_fd(ssl_connection, network_socket) == 0){
         g_printerr("Error connecting network socket to SSL\n");
         exit_error = 1;
     }
   }

   if (exit_error == 0){
      /*Actually do SSL connection handshake*/
      if (SSL_connect(ssl_connection) != 1){
         g_printerr("SSL_connect failed\n");
         exit_error = 1;
      }
   }

   /*Use handle error function to decide what to do*/
   if (exit_error == 0){
      if (handle_certificate_problem(ssl_connection) < 0){
         g_printerr("Certificate verification error\n");
         exit_error = 1;
      }
   }
#endif

   if (exit_error == 0) {
      char *d_cmd;

      /*Send query we want*/
#ifdef ENABLE_AXTLS
      ssl_write(ssl_connection,
                (const uint8_t*) http_query,
                (int)strlen(http_query));
#else
      SSL_write(ssl_connection, http_query, (int)strlen(http_query));
#endif

      /*Analyse response from server*/

      /*Send dpi command*/
      d_cmd = a_Dpip_build_cmd("cmd=%s url=%s", "start_send_page", url);
      sock_handler_write_str(sh, d_cmd, 1);
      g_free(d_cmd);

      /*Send remaining data*/

#ifdef ENABLE_AXTLS
      if (SSL_OK == ssl_read(ssl_connection, &buf)) {
         while ((retval = ssl_read(ssl_connection, &buf)) > 0 ){
            sock_handler_write(sh, (const char*)buf, (size_t)retval, 0);
         }
      }
#else
      while ((retval = SSL_read(ssl_connection, buf, 4096)) > 0 ){
         sock_handler_write(sh, buf, (size_t)retval, 0);
      }
#endif
   }

   /*Begin cleanup of all resources used*/
   g_free(dpip_tag);
   g_free(cmd);
   g_free(url);
   g_free(http_query);

   if (network_socket != -1){
      close(network_socket);
      network_socket = -1;
   }
   if (ssl_connection != NULL){
#ifdef ENABLE_AXTLS
      ssl_free(ssl_connection);
#else
      SSL_free(ssl_connection);
#endif
      ssl_connection = NULL;
   }
   if (ssl_context != NULL){
#ifdef ENABLE_AXTLS
      ssl_ctx_free(ssl_context);
#else
      SSL_CTX_free(ssl_context);
#endif
      ssl_context = NULL;
   }
}

/*
 * The following function attempts to open up a connection to the
 * remote server and return the file descriptor number of the
 * socket.  Returns -1 in the event of an error
 */
#ifdef ENABLE_SSL
static int get_network_connection(gchar * url)
{
   struct sockaddr_in address;
   struct hostent *hp;

   int s;
   int url_offset = 0;
   int portnum = 443;
   unsigned int url_look_up_length = 0;
   gchar * url_look_up = NULL;

   /*Determine how much of url we chop off as unneeded*/
   if (g_strncasecmp(url, "https://", 8) == 0){
      url_offset = 8;
   }

   /*Find end of URL*/

   if (strpbrk(url+url_offset, ":/") != NULL){
      url_look_up_length = strpbrk(url+url_offset, ":/") - (url+url_offset);
      url_look_up = g_strndup(url+url_offset, url_look_up_length);

      /*Check for port number*/
      if (strchr(url+url_offset, ':') ==
          (url + url_offset + url_look_up_length)){
         portnum = atoi(url + url_offset + url_look_up_length + 1);
      }
   } else {
      url_look_up = url + url_offset;
   }

   root_url = g_strdup(url_look_up);
   hp=gethostbyname(url_look_up);

   /*url_look_uip no longer needed, so free if neccessary*/
   if (url_look_up_length != 0){
      g_free(url_look_up);
   }

   if (hp == NULL){
      g_printerr("gethostbyname() failed\n");
      return -1;
   }

   memset(&address,0,sizeof(address));
   memcpy((char *)&address.sin_addr, hp->h_addr, (size_t)hp->h_length);
   address.sin_family=hp->h_addrtype;
   address.sin_port= htons((u_short)portnum);

   s = socket(hp->h_addrtype, SOCK_STREAM, 0);
   if (connect(s, (struct sockaddr *)&address, sizeof(address)) != 0){
      close(s);
      s = -1;
      g_printerr("errno: %i\n", errno);
   }
   return s;
}
#endif

/* This function is run only when the certificate cannot
 * be completely trusted.  This will notify the user and
 * allow the user to decide what to do.  It may save the
 * certificate to the user's .dillo directory if it is
 * trusted.
 * Return value: -1 on abort, 0 or higher on continue
 */
static int handle_certificate_problem(SSL * ssl_connection)
{
#ifdef ENABLE_AXTLS
   return (-1);
#else
   gint response_number;
   int retval = -1;
   long st;
   char *cn, *cn_end;
   char buf[4096], *d_cmd, *msg;

   X509 * remote_cert;

   remote_cert = SSL_get_peer_certificate(ssl_connection);
   if (remote_cert == NULL){
      /*Inform user that remote system cannot be trusted*/
      d_cmd = a_Dpip_build_cmd(
         "cmd=%s msg=%s alt1=%s alt2=%s",
         "dialog",
         "The remote system is NOT presenting a certificate.\n"
         "This site CAN NOT be trusted. Sending data is NOT SAFE.\n"
         "What do I do?",
         "Continue", "Cancel");
      sock_handler_write_str(sh, d_cmd, 1);
      g_free(d_cmd);

      /*Read the user's response*/
      response_number = dialog_get_answer_number();

      /*Abort on anything but "Continue"*/
      if (response_number == 1){
         retval = 0;
      }

   } else {
      /*Figure out if (and why) the remote system can't be trusted*/
      st = SSL_get_verify_result(ssl_connection);
      switch (st) {
      case X509_V_OK:      /*Everything is Kosher*/
         retval = 0;
         break;
      case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
         /*Either self signed and untrusted*/
         /*Extract CN from certificate name information*/
         cn = strstr(remote_cert->name, "/CN=") + 4;
         if (cn == NULL)
            break;

         if ((cn_end = strstr(cn, "/")) == NULL )
            cn_end = cn + strlen(cn);

         strncpy(buf, cn, (size_t) (cn_end - cn));

         /*Add terminating NULL*/
         buf[cn_end - cn] = 0;

         msg = g_strdup_printf("The remote certificate is self-signed and "
                               "untrusted.\nFor address: %s", buf);
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s alt3=%s",
            "dialog", msg, "Continue", "Cancel", "Trust Certificate");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);
         g_free(msg);

         response_number = dialog_get_answer_number();
         switch (response_number){
            case 1:
               retval = 0;
               break;
            case 2:
               break;
            case 3:
               /*Save certificate to a file here and recheck the chain*/
               /*Potential security problems because we are writing
                *to the filesystem*/
               save_certificate_home(remote_cert);
               retval = 1;
               break;
            default:
               break;
         }
         break;
      case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
      case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s",
            "dialog",
            "The issuer for the remote certificate cannot be found\n"
            "The authenticity of the remote certificate cannot be trusted",
            "Continue", "Cancel");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);

         response_number = dialog_get_answer_number();
         if (response_number == 1) {
            retval = 0;
         }
         break;

      case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
      case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
      case X509_V_ERR_CERT_SIGNATURE_FAILURE:
      case X509_V_ERR_CRL_SIGNATURE_FAILURE:
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s",
            "dialog",
            "The remote certificate signature could not be read\n"
            "or is invalid and should not be trusted",
            "Continue", "Cancel");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);

         response_number = dialog_get_answer_number();
         if (response_number == 1) {
            retval = 0;
         }
         break;
      case X509_V_ERR_CERT_NOT_YET_VALID:
      case X509_V_ERR_CRL_NOT_YET_VALID:
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s",
            "dialog",
            "Part of the remote certificate is not yet valid\n"
            "Certificates usually have a range of dates over which\n"
            "they are to be considered valid, and the certificate\n"
            "presented has a starting validity after today's date\n"
            "You should be cautious about using this site",
            "Continue", "Cancel");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);

         response_number = dialog_get_answer_number();
         if (response_number == 1) {
            retval = 0;
         }
         break;
      case X509_V_ERR_CERT_HAS_EXPIRED:
      case X509_V_ERR_CRL_HAS_EXPIRED:
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s",
            "dialog",
            "The remote certificate has expired.  The certificate\n"
            "wasn't designed to last this long. You should avoid \n"
            "this site.",
            "Continue", "Cancel");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);
         response_number = dialog_get_answer_number();
         if (response_number == 1) {
            retval = 0;
         }
         break;
      case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
      case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
      case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
      case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s",
            "dialog",
            "There was an error in the certificate presented.\n"
            "Some of the certificate data was improperly formatted\n"
            "making it impossible to determine if the certificate\n"
            "is valid.  You should not trust this certificate.",
            "Continue", "Cancel");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);
         response_number = dialog_get_answer_number();
         if (response_number == 1) {
            retval = 0;
         }
         break;
      case X509_V_ERR_INVALID_CA:
      case X509_V_ERR_INVALID_PURPOSE:
      case X509_V_ERR_CERT_UNTRUSTED:
      case X509_V_ERR_CERT_REJECTED:
      case X509_V_ERR_KEYUSAGE_NO_CERTSIGN:
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s",
            "dialog",
            "One of the certificates in the chain is being used\n"
            "incorrectly (possibly due to configuration problems\n"
            "with the remote system.  The connection should not\n"
            "be trusted",
            "Continue", "Cancel");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);
         response_number = dialog_get_answer_number();
         if (response_number == 1) {
            retval = 0;
         }
         break;
      case X509_V_ERR_SUBJECT_ISSUER_MISMATCH:
      case X509_V_ERR_AKID_SKID_MISMATCH:
      case X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH:
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s",
            "dialog",
            "Some of the information presented by the remote system\n"
            "does not match other information presented\n"
            "This may be an attempt to evesdrop on communications",
            "Continue", "Cancel");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);
      default:             /*Need to add more options later*/
         msg = g_strdup_printf(
            "The remote certificate cannot be verified (code %ld)", st);
         d_cmd = a_Dpip_build_cmd(
            "cmd=%s msg=%s alt1=%s alt2=%s",
            "dialog", msg, "Continue", "Cancel");
         sock_handler_write_str(sh, d_cmd, 1);
         g_free(d_cmd);
         g_free(msg);
         response_number = dialog_get_answer_number();
         /*abort on anything but "Continue"*/
         if (response_number == 1){
            retval = 0;
         }
      }
      X509_free(remote_cert);
      remote_cert = 0;
   }

   return retval;
#endif
}

/*
 * Save certificate with a hashed filename.
 * Return: 0 on success, 1 on failure.
 */
#ifndef ENABLE_AXTLS
static int save_certificate_home(X509 * cert)
{
   char buf[4096];

   FILE * fp = NULL;
   unsigned int i = 0;
   int retval = 1;

   /*Attempt to create .dillo/certs blindly - check later*/
   g_snprintf(buf,4096,"%s/.dillo/", g_get_home_dir());
   mkdir(buf, 01777);
   g_snprintf(buf,4096,"%s/.dillo/certs/", g_get_home_dir());
   mkdir(buf, 01777);

   do{
      g_snprintf(buf,4096,"%s/.dillo/certs/%lx.%u",
         g_get_home_dir(), X509_subject_name_hash(cert), i);

      fp=fopen(buf, "r");
      if (fp == NULL){
         /*File name doesn't exist so we can use it safely*/
         fp=fopen(buf, "w");
         if (fp == NULL){
            g_printerr("Unable to open cert save file in home dir\n");
            break;
         } else {
            PEM_write_X509(fp, cert);
            fclose(fp);
            g_printerr("Wrote certificate\n");
            retval = 0;
            break;
         }
      } else {
         fclose(fp);
      }
      i++;
      /*Don't loop too many times - just give up*/
   } while( i < 1024 );

   return retval;
}


#endif

#else


/*
 * Call this function to display an error message if SSL support
 * isn't available for some reason
 */
static void no_ssl_support(void)
{
   gchar *dpip_tag = NULL, *cmd = NULL, *url = NULL, *http_query = NULL;
   char *d_cmd;

   /* Read the dpi command from STDIN */
   dpip_tag = sock_handler_read(sh);

   g_printerr("{In https.filter.dpi}\n");
   g_printerr("no_ssl_support version\n");

   cmd = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "cmd");
   url = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "url");
   http_query = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "query");

   g_printerr("{ cmd: %s}\n", cmd);
   g_printerr("{ url: %s}\n", url);
   g_printerr("{ http_query:\n%s}\n", http_query);

   g_printerr("{ sending dpip cmd...}\n");

   d_cmd = a_Dpip_build_cmd("cmd=%s url=%s", "start_send_page", url);
   sock_handler_write_str(sh, d_cmd, 1);
   g_free(d_cmd);

   g_printerr("{ dpip cmd sent.}\n");

   g_printerr("{ sending HTML...}\n");

   sock_handler_printf(sh, 1,
      "Content-type: text/html\n\n"
      "<!DOCTYPE HTML PUBLIC '-//W3C//DTD HTML 4.01 Transitional//EN'>\n"
      "<html><body><pre>\n"
      "<b>Hi!\n\n"
      "  This is the https dpi that just got a request to send\n"
      "  the following HTTP query:\n{</b>\n"
      "<code>%s</code>\n"
      "<b>}</b>\n\n"
      "  <b>*** Dillo's prototype plugin for https support"
      " is disabled now ***</b>\n\n"
      "  If you want to test this <b>alpha</b> support code, just remove\n"
      "  line 65 from https.c, recompile and reinstall.\n\n"
      "  (beware that this https support is very limited now)\n\n"
      "  To use https and SSL, you must have \n"
      "  the OpenSSL development libraries installed.  Check your\n"
      "  O/S distribution provider, or check out\n"
      "  <a href=\"http://www.openssl.org\">www.openssl.org</a>\n\n"
      "  --\n"
      "</pre></body></html>\n",
      http_query
   );
   g_printerr("{ HTML content sent.}\n");

   g_free(cmd);
   g_free(url);
   g_free(http_query);
   g_free(dpip_tag);

   g_printerr("{ exiting https.dpi}\n");

}

#endif


/*---------------------------------------------------------------------------*/
int main(void)
{
   /* Initialize the SockHandler for this filter dpi */
   sh = sock_handler_new(STDIN_FILENO, STDOUT_FILENO, 8*1024);

#ifdef ENABLE_SSL
   yes_ssl_support();
#else
   no_ssl_support();
#endif

   /* Finish the SockHandler */
   sock_handler_close(sh);
   sock_handler_free(sh);

   g_free(root_url);

   g_printerr("{ exiting https.dpi}\n");

   return 0;
}

