/*
 * shar.c -- the SHAR "main" code.  All the functionality is elsewhere
 *           (in case you want to turn this into a library later).
 *
 * Created By:	Derek Atkins <derek@ihtfp.com>
 *
 * $Id$
 */

#include <unistd.h>
#include <stdio.h>
#include <sys/select.h>
#include <errno.h>

#include <shib-target/shib-target.h>

typedef struct {
  u_long prog;
  u_long vers;
  void (*dispatch)();
} ShibRPCProtocols;

extern void shibrpc_prog_1(struct svc_req *, SVCXPRT *);
static int shar_run = 1;

static int
new_connection (ShibSocket listener, const ShibRPCProtocols protos[], int numproto)
{
  int i;
  SVCXPRT *svc;
  ShibSocket sock;

  /* Accept the connection */
  if (shib_sock_accept (listener, &sock)) {
    fprintf (stderr, "ACCEPT failed\n");
    return -1;
  }

  /* Wrap an RPC Service around the new connection socket */
  svc = svcfd_create (sock, 0, 0);
  if (!svc) {
    fprintf (stderr, "Cannot create RPC Listener\n");
    return -2;
  }

  /* Register the SHIBRPC RPC Program */
  for (i = 0; i < numproto; i++) {
    if (!svc_register (svc, protos[i].prog, protos[i].vers,
		       protos[i].dispatch, 0)) {
      svc_destroy(svc);
      shib_sock_close (sock);
      fprintf (stderr, "Cannot register RPC Program\n");
      return -3;
    }
  }
  return 0;
}

static void
shar_svc_run (ShibSocket listener, const ShibRPCProtocols protos[], int numproto)
{
  while (shar_run) {
    fd_set readfds = svc_fdset;
    struct timeval tv = { 0, 0 };

    FD_SET(listener, &readfds);
    tv.tv_sec = 5;

    switch (select (getdtablesize(), &readfds, 0, 0, &tv)) {

    case -1:
      if (errno == EINTR) continue;
      perror ("shar_svc_run: - select failed");
      return;

    case 0:
      continue;

    default:
      if (FD_ISSET (listener, &readfds)) {
	new_connection (listener, protos, numproto);
	FD_CLR (listener, &readfds);
      }

      svc_getreqset (&readfds);
    }
  }
}

int
main (int argc, char *argv[])
{
  ShibSocket sock;
  char* config = getenv("SHIBCONFIG");
  ShibRPCProtocols protos[] = {
    { SHIBRPC_PROG, SHIBRPC_VERS_1, shibrpc_prog_1 }
  };

  /* initialize the shib-target library */
  if (shib_target_initialize(SHIBTARGET_SHAR, config))
    return -1;

  /* Create the SHAR listener socket */
  if (shib_sock_create (&sock) != 0)
    return -2;

  /* Bind to the proper port */
  if (shib_sock_bind (sock, SHIB_SHAR_SOCKET) != 0)
    return -3;

  shar_svc_run(sock, protos, 1);

  shib_sock_close(sock);
  fprintf (stderr, "shar_svc_run returned.\n");
  return 0;

  /* XXX: the user may have to remove the SHAR Socket */
}
