/*
 *	appl/bsd/krshd.c
 */

/*
 * Copyright (c) 1983 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef lint
char copyright[] =
  "@(#) Copyright (c) 1983 The Regents of the University of California.\n\
 All rights reserved.\n";
#endif /* not lint */

/* based on @(#)rshd.c	5.12 (Berkeley) 9/12/88 */

     /*
      * remote shell server:
      *	remuser\0
      *	locuser\0
      *	command\0
      *	data
      */
     
/*
 * This is the rshell daemon. The very basic protocol for checking 
 * authentication and authorization is:
 * 1) Check authentication.
 * 2) Check authorization via the access-control files: 
 *    ~/.k5login (using krb5_kuserok) and/or
 *    ~/.rhosts  (using ruserok).
 * Execute command if configured authoriztion checks pass, else deny 
 * permission.
 *
 * The configuration is done either by command-line arguments passed by inetd, 
 * or by the name of the daemon. If command-line arguments are present, they 
 * take priority. The options are:
 * -k and -K means check .k5login (using krb5_kuserok).
 * -r and -R means check .rhosts  (using ruserok).
 * The difference between upper and lower case is as follows:
 *    If lower case -r or -k, then as long as one of krb5_kuserok or ruserok 
 * passes, allow access. If both fail, no access. The program does not fall
 * back on password verification.
 *    If uppercase -R or -K, then those checks must be passed, regardless of 
 * other checks, else no access.
 * 
 *     If no command-line arguments are present, then the presence of the 
 * letters kKrR in the program-name before "shd" determine the 
 * behaviour of the program exactly as with the command-line arguments.
 */
     
/* DEFINES:
 *   KERBEROS - Define this if application is to be kerberised.
 *   KRB5_KRB4_COMPAT - Define this if v4 rlogin clients are also to be served.
 *   ALWAYS_V5_KUSEROK - Define this if you want .k5login to be
 *              checked even for v4 clients (instead of .klogin).
 *   LOG_ALL_LOGINS - Define this if you want to log all logins.
 *   LOG_OTHER_USERS - Define this if you want to log all principals that do
 *              not map onto the local user.
 *   LOG_REMOTE_REALM - Define this if you want to log all principals from 
 *              remote realms.
 *   LOG_CMD - Define this if you want to log not only the user but also the
 *             command executed. This only decides the type of information
 *             logged. Whether or not to log is still decided by the above 
 *             three DEFINES.
 *       Note:  Root account access is always logged.
 */
     
#define SERVE_NON_KRB     
#define LOG_REMOTE_REALM
#define LOG_CMD
     
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef __SCO__
#include <sys/unistd.h>
#endif

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/time.h>

#include <fcntl.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
     
#include <netinet/in.h>
#include <arpa/inet.h>
     
#include <stdio.h>
#include <errno.h>
#include <pwd.h>
#include <ctype.h>
#include <string.h>
     
#ifdef HAVE_SYS_LABEL_H
/* only SunOS 4? */
#include <sys/label.h>
#include <sys/audit.h>
#include <pwdadj.h>
#endif
     
#include <signal.h>
#include <netdb.h>
     
#ifdef CRAY
#ifndef NO_UDB
#include <udb.h>
#endif  /* !NO_UDB */
#include <sys/category.h>
#include <netinet/ip.h>
#include <sys/tfm.h>
#include <sys/nal.h>
#include <sys/secparm.h>
#include <sys/usrv.h>
#include <sys/utsname.h>
#include <sys/sysv.h>
#include <sys/slrec.h>
#include <sys/unistd.h>
#include <path.h>
#endif /* CRAY */
     
#include <syslog.h>

#ifdef POSIX_TERMIOS
#include <termios.h>
#endif
     
#ifdef HAVE_SYS_FILIO_H
/* get FIONBIO from sys/filio.h, so what if it is a compatibility feature */
#include <sys/filio.h>
#endif

#ifdef KERBEROS
#include <krb5/krb5.h>
#include <krb5/asn1.h>
#include <krb5/crc-32.h>
#include <krb5/mit-des.h>
#include <krb5/ext-proto.h>

#include <com_err.h>
#include "loginpaths.h"

#define ARGSTR	"rRxXeEkKD:?"

#define SECURE_MESSAGE "This rsh session is using DES encryption for all data transmissions.\r\n"

#ifdef BUFSIZ
#undef BUFSIZ
#endif
#define BUFSIZ 5120

#define MAXRETRIES 4

char des_inbuf[2*BUFSIZ];         /* needs to be > largest read size */
krb5_encrypt_block eblock;        /* eblock for encrypt/decrypt */
char des_outbuf[2*BUFSIZ];        /* needs to be > largest write size */
krb5_data desinbuf,desoutbuf;
krb5_context bsd_context;

void fatal();
int v5_des_read();
int v5_des_write();

int (*des_read)() = v5_des_read;
int (*des_write)() = v5_des_write;

int do_encrypt = 0;
int netf;

#else /* !KERBEROS */

#define ARGSTR	"rRD:?"
#define (*des_read)  read
#define (*des_write) write
     
#endif /* KERBEROS */
     
#ifndef HAVE_KILLPG
#define killpg(pid, sig) kill(-(pid), (sig))
#endif

int must_pass_rhosts = 0, must_pass_k5 = 0, must_pass_one = 0;
int failed_k5 = 0;
char *progname;

#define MAX_PROG_NAME 10

#ifdef CRAY
int     secflag;
extern
#endif /* CRAY */
int     errno;

/*VARARGS1*/
int	error();


#ifdef __SCO__
/* sco has getgroups and setgroups but no initgroups */
int initgroups(char* name, gid_t basegid) {
  gid_t others[NGROUPS_MAX+1];
  int ngrps;

  others[0] = basegid;
  ngrps = getgroups(NGROUPS_MAX, others+1);
  return setgroups(ngrps+1, others);
}
#endif


main(argc, argv)
     int argc;
     char **argv;
{
#if defined(BSD) && BSD+0 >= 43
    struct linger linger;
#endif
    int on = 1, fromlen;
    struct sockaddr_in from;
    extern int opterr, optind;
    extern char *optarg;
    char *options;
    int ch;
    int i;
    int fd;
    int debug_port = 0;

#ifdef CRAY
    secflag = sysconf(_SC_CRAY_SECURE_SYS);
#endif
    
    progname = *argv;
    
#ifndef LOG_ODELAY /* 4.2 syslog */
    openlog(progname, LOG_PID);
#else
#ifndef LOG_DAEMON
#define LOG_DAEMON 0
#endif
    openlog(progname, LOG_PID | LOG_ODELAY, LOG_DAEMON);	
#endif /* 4.2 syslog */
    
    if (argc == 1) { /* Get parameters from program name. */
	if (strlen(progname) > MAX_PROG_NAME) {
	    usage();
	    exit(1);
	}
	options = (char *) malloc(MAX_PROG_NAME+1);
	options[0] = '\0';
	for (i = 0; (progname[i] != '\0') && (i < MAX_PROG_NAME); i++)
	  if (!strcmp(progname+i, "shd")) {
	      strcpy(options, "-");
	      strncat(options, progname, i);
	      options[i+1] = '\0';
#if 0
	      /*
	       * Since we are just going to break out afterwards, we'll
	       * re-use the variable "i" to move the command line args.
	       */
	      for (i=argc-1; i>0; i--) argv[i+1] = argv[i];
#endif
	      argv[++argc] = NULL;

	      argv[1] = options;
	      break;
	  }
    }
    
    /* Analyze parameters. */
    opterr = 0;
    while ((ch = getopt(argc, argv, ARGSTR)) != EOF)
      switch (ch) {
	case 'r':         
	  must_pass_one = 1; /* If just 'r', any one check must succeed */
	  break;
	case 'R':         /* If 'R', must pass .rhosts check*/
	  must_pass_rhosts = 1;
	  if (must_pass_one)
	    must_pass_one = 0;
	  break;
#ifdef KERBEROS
	case 'k':
	  must_pass_one = 1; /* If just 'k', any one check must succeed */
	  break;
	case 'K':         /* If 'K', must pass .k5login check*/
	  must_pass_k5 = 1;
	  if (must_pass_one)
	    must_pass_one = 0;
	  break;
	  
        case 'x':
        case 'X':
        case 'e':
        case 'E':
	  do_encrypt = 1;
	  break;
#endif
	case 'D':
	  debug_port = atoi(optarg);
	  break;
	case '?':
	default:
	  usage();
	  exit(1);
	  break;
      }

    if (optind == 0) {
	usage();
	exit(1);
    }
    
    argc -= optind;
    argv += optind;
    
    fromlen = sizeof (from);

    if (debug_port) {
	int s;
	struct sockaddr_in sin;
	
	if ((s = socket(AF_INET, SOCK_STREAM, PF_UNSPEC)) < 0) {
	    fprintf(stderr, "Error in socket: %s\n", strerror(errno));
	    exit(2);
	}
	
	memset((char *) &sin, 0,sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(debug_port);
	sin.sin_addr.s_addr = INADDR_ANY;
	
	if ((bind(s, (struct sockaddr *) &sin, sizeof(sin))) < 0) {
	    fprintf(stderr, "Error in bind: %s\n", strerror(errno));
	    exit(2);
	}
	
	if ((listen(s, 5)) < 0) {
	    fprintf(stderr, "Error in listen: %s\n", strerror(errno));
	    exit(2);
	}
	
	if ((fd = accept(s, &from, &fromlen)) < 0) {
	    fprintf(stderr, "Error in accept: %s\n", strerror(errno));
	    exit(2);
	}
	
	close(s);
    } else {
	if (getpeername(0, (struct sockaddr *)&from, &fromlen) < 0) {
	    fprintf(stderr, "%s: ", progname);
	    perror("getpeername");
	    _exit(1);
	}
	
	fd = 0;
    }
    
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&on,
		   sizeof (on)) < 0)
      syslog(LOG_WARNING,
	     "setsockopt (SO_KEEPALIVE): %m");
#if defined(BSD) && BSD+0 >= 43
    linger.l_onoff = 1;
    linger.l_linger = 60;			/* XXX */
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char *)&linger,
		   sizeof (linger)) < 0)
      syslog(LOG_WARNING , "setsockopt (SO_LINGER): %m");
#endif
    doit(dup(fd), &from);
}

#ifdef CRAY
char    username[32] = "LOGNAME=";
#include <tmpdir.h>
char tmpdir[64] = "TMPDIR=";
#else
char	username[20] = "USER=";
#endif

char	homedir[64] = "HOME=";
char	shell[64] = "SHELL=";
char    term[64] = "TERM=network";
char	path[128] = "PATH=";
char	path_rest[64] = RPATH;

#ifdef CRAY
char    *envinit[] =
{homedir, shell, path, username, "TZ=GMT0", tmpdir, term, 0};
#define TZENV   4
#define TMPDIRENV 5
char    *getenv();
#else /* CRAY */
#ifdef KERBEROS
char    *envinit[] =
{homedir, shell, path, username, term, 0};
#else /* KERBEROS */
char	*envinit[] =
{homedir, shell, path, username, term, 0};
#endif /* KERBEROS */
#endif /* CRAY */

extern char	**environ;
char ttyn[12];		/* Line string for wtmp entries */

#ifdef CRAY
#define SIZEOF_INADDR  SIZEOF_in_addr
int maxlogs;
#else
#define SIZEOF_INADDR sizeof(struct in_addr)
#endif

#ifndef NCARGS
/* linux doesn't seem to have it... */
#define NCARGS 1024
#endif

#define NMAX   16 

int pid;
char locuser[NMAX+1];
char remuser[NMAX +1];
char cmdbuf[NCARGS+1];
char *kremuser;
krb5_principal client;
krb5_authenticator *kdata;

#include <kerberosIV/krb.h>
AUTH_DAT	*v4_kdata;
KTEXT		v4_ticket;

int auth_sys = 0;	/* Which version of Kerberos used to authenticate */

#define KRB5_RECVAUTH_V4	4
#define KRB5_RECVAUTH_V5	5

doit(f, fromp)
     int f;
     struct sockaddr_in *fromp;
{
    char *cp;
    
#ifdef KERBEROS
    krb5_address peeraddr;
    krb5_error_code status;
#endif

    int tmpint;
    
    int ioctlval, cnt;
    char *salt, *ttynm, *tty;
    register char *p;
    char *crypt();
    struct passwd *pwd;
    
#ifdef CRAY
#ifndef NO_UDB
    struct udb    *ue;
    struct udb ue_static;
    extern struct udb *getudbnam();
#endif
    extern struct passwd *getpwnam(), *getpwuid();
    static int      jid;
    int error();
    int paddr;
    struct  nal nal;
    int     nal_error;
    struct usrv usrv;
    struct  sysv sysv;
    char    *makejtmp(), *jtmpnam = 0;
    int packet_level;               /* Packet classification level */
    long packet_compart;            /* Packet compartments */
#endif  /* CRAY */
    
    int s;
    struct hostent *hp;
    char *hostname;
    short port;
    int pv[2], pw[2], px[2], cc;
    fd_set ready, readfrom;
    char buf[BUFSIZ], sig;
    int one = 1;
    krb5_sigtype     cleanup();
    int fd;
    struct sockaddr_in fromaddr;
    int non_privileged = 0;
#ifdef POSIX_SIGNALS
    struct sigaction sa;
#endif

#ifdef IP_TOS
/* solaris has IP_TOS, but only IPTOS_* values */
#ifdef HAVE_GETTOSBYNAME
    struct tosent *tp;

    if ((tp = gettosbyname("interactive", "tcp")) &&
	(setsockopt(f, IPPROTO_IP, IP_TOS, &tp->t_tos, sizeof(int)) < 0))
#ifdef  TOS_WARN
      syslog(LOG_NOTICE, "setsockopt (IP_TOS): %m");
#else
    ;       /* silently ignore TOS errors in 6E */
#endif
#endif
#endif /* IP_TOS */
    
    fromaddr = *fromp;

#ifdef POSIX_SIGNALS
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;
    (void)sigaction(SIGINT, &sa, (struct sigaction *)0);
    (void)sigaction(SIGQUIT, &sa, (struct sigaction *)0);
    (void)sigaction(SIGTERM, &sa, (struct sigaction *)0);
#else
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
#endif
#ifdef DEBUG
    { int t = open("/dev/tty", 2);
      if (t >= 0) {
	  ioctl(t, TIOCNOTTY, (char *)0);
	  (void) close(t);
      }
  }
#endif
    fromp->sin_port = ntohs((u_short)fromp->sin_port);
    if (fromp->sin_family != AF_INET) {
	syslog(LOG_ERR , "malformed from address\n");
	exit(1);
    }
#ifdef KERBEROS
    krb5_init_context(&bsd_context);
    krb5_init_ets(bsd_context);
    netf = f;
    desinbuf.data = des_inbuf;
    desoutbuf.data = des_outbuf;
    if ((must_pass_rhosts || must_pass_one)
	&& (fromp->sin_port >= IPPORT_RESERVED ||
	    fromp->sin_port < IPPORT_RESERVED/2))
      non_privileged = 1;
#else
    if (fromp->sin_port >= IPPORT_RESERVED ||
	    fromp->sin_port < IPPORT_RESERVED/2) {
	syslog(LOG_ERR , "connection from bad port\n");
	exit(1);
    }
#endif /* KERBEROS */
    
#ifdef CRAY
    
    /* If this is a secure system then get the packet classification
       of f.  ( Note IP_SECURITY is checked in get_packet_classification:
       if it's not set then the user's (root) default
       classification level and compartments are returned. )
       Then set this process to that level/compart so that the stderr
       connection will be labeled appropriately.
       */
    if (secflag) {
	if (get_packet_classification(f,getuid(),
				      &packet_level,&packet_compart) < 0) {
	    syslog(LOG_ERR, "cannot get ip packet level\n");
	    exit(1);
	}
	if(secflag == TFM_UDB_5) {
	    if(setucmp(packet_compart, C_PROC) != 0) {
		error("Unable to setucmp.\n");
		exit(1);
	    }
	} else if(secflag == TFM_UDB_6) {
	    if(setulvl(packet_level,C_PROC) != 0) {
		error("Unable to setulvl.\n");
		exit(1);
	    }
	    if(setucmp(packet_compart, C_PROC) != 0) {
		error("Unable to setucmp.\n");
		exit(1);
	    }
	}
	
    }
#endif /* CRAY */
    
    (void) alarm(60);
    port = 0;
    for (;;) {
	char c;
	if ((cc = read(f, &c, 1)) != 1) {
	    if (cc < 0)
	      syslog(LOG_NOTICE , "read: %m");
	    shutdown(f, 1+1);
	    exit(1);
	}
	if (c == 0)
	  break;
	port = port * 10 + c - '0';
    }
    (void) alarm(0);
    if (port != 0) {
	int lport = IPPORT_RESERVED - 1;
	s = rresvport(&lport);
	if (s < 0) {
	    syslog(LOG_ERR ,
		   "can't get stderr port: %m");
	    exit(1);
	}
#ifdef KERBEROS
	if ((must_pass_rhosts || must_pass_one)
	    && port >= IPPORT_RESERVED)
	  non_privileged = 1;
#else
	if (port >= IPPORT_RESERVED) {
	    syslog(LOG_ERR , "2nd port not reserved\n");
	    exit(1);
	}
#endif /* KERBEROS */
	fromp->sin_port = htons((u_short)port);
	if (connect(s, (struct sockaddr *)fromp, sizeof (*fromp)) < 0) {
	    syslog(LOG_INFO ,
		   "connect second port: %m");
	    exit(1);
	}
    }
    dup2(f, 0);
    dup2(f, 1);
    dup2(f, 2);
    hp = gethostbyaddr((char *)&fromp->sin_addr, sizeof (struct in_addr),
		       fromp->sin_family);
    if (hp){
	hostname = malloc(strlen(hp->h_name) + 1);
	strcpy(hostname,hp->h_name);
    }
    else {
	hostname = malloc(strlen((char *)inet_ntoa(fromp->sin_addr)) + 1);
	strcpy(hostname,(char *)inet_ntoa(fromp->sin_addr));
    }
    peeraddr.addrtype = fromp->sin_family;
    peeraddr.length = SIZEOF_INADDR;
    peeraddr.contents = (krb5_octet *)&fromp->sin_addr;


#ifdef KERBEROS
    if (status = recvauth(f, fromaddr, peeraddr)) {
	error("Authentication failed: %s\n", error_message(status));
	exit(1);
    }
    if (!strncmp(cmdbuf, "-x ", 3))
	do_encrypt = 1;
#else
    getstr(f, remuser, sizeof(remuser), "remuser");
    getstr(f, locuser, sizeof(locuser), "locuser");
    getstr(f, cmdbuf, sizeof(cmdbuf), "command");
#endif /* KERBEROS */
    
#ifdef CRAY
    paddr = inet_addr(inet_ntoa(fromp->sin_addr));
    if(secflag){
	/*
	 *      check network authorization list
	 */
	if (fetchnal(paddr,&nal) < 0) {
	    /*
	     *      NAL file inaccessible, abort connection.
	     */
	    error("Permission denied.\n");
	    exit(1);
	}
    }
#endif /* CRAY */
    
    pwd = getpwnam(locuser);
    if (pwd == (struct passwd *) 0 ) {
	syslog(LOG_ERR ,
	       "Principal %s (%s@%s) for local user %s has no account.\n",
	       kremuser, remuser, hostname, locuser);
	error("Login incorrect.\n");
	exit(1);
    }
    
#ifdef CRAY
    /* Setup job entry, and validate udb entry. 
       ( against packet level also ) */
    if ((jid = setjob(pwd->pw_uid, 0)) < 0) {
	error("Unable to create new job.\n");
	exit(1);
    }
    if ((jtmpnam = makejtmp(pwd->pw_uid, pwd->pw_gid, jid))) {
	register int pid, tpid;
	int status;
	switch(pid = fork()) {
	  case -1:
	    cleanjtmp(locuser, jtmpnam);
	    envinit[TMPDIRENV] = 0;
	    break;
	  case 0:
	    break;
	  default:
	    close(0);
	    close(1);
	    close(2);
	    close(f);
	    if (port)
	      close(s);
	    while ((tpid = wait(&status)) != pid) {
		if (tpid < 0)
		  break;
	    }
	    cleanjtmp(locuser, jtmpnam);
	    exit(status>>8);
	    /* NOTREACHED */
	}
    } else {
	envinit[TMPDIRENV] = 0;
    }
#ifndef NO_UDB
    (void)getsysudb();
    
    if ((ue = getudbnam(pwd->pw_name)) == (struct udb *)NULL) {
	error("Unable to fetch account id.\n");
	exit(1);
    }
    ue_static = *ue;                /* save from setlimits call */
    endudb();
    if (secflag) {
	if(getsysv(&sysv, sizeof(struct sysv)) != 0) {
	    loglogin(hostname, SLG_LLERR, 0, ue);
	    error("Permission denied.\n");
	    exit(1);
	}
	if ((packet_level != ue->ue_deflvl) ||
	    ((packet_compart & ue->ue_comparts) != packet_compart )){
	    loglogin(hostname, SLG_LLERR, 0, ue);
	    error("Permission denied.\n");
	    exit(1);
	}
	if (ue->ue_disabled != 0) {
	    loglogin(hostname,SLG_LOCK,ue->ue_logfails,ue);
	    error("Permission denied.\n");
	    exit(1);
	}
	maxlogs = sysv.sy_maxlogs;
    }
    if (acctid(getpid(), ue->ue_acids[0]) == -1) {
	error("Unable to set account id.\n");
	exit(1);
    }
    if (setshares(pwd->pw_uid, acctid(0, -1), error, 1, 0)) {
	error("Unable to set shares.\n");
	exit(1);
    }
    if (setlimits(pwd->pw_name, C_PROC, getpid(), UDBRC_INTER)) {
	error("Unable to set limits.\n");
	exit(1);
    }
    if (setlimits(pwd->pw_name, C_JOB, jid, UDBRC_INTER)) {
	error("Unable to set limits.\n");
	exit(1);
    }
    ue = &ue_static;                /* restore after setlimits call */
    endudb();			/* setlimits opens udb and leaves it
				   open so close it here. */
#endif  /* !NO_UDB */
#endif /*CRAY*/
    
    /* Setup wtmp entry : we do it here so that if this is a CRAY
       the Process Id is correct and we have not lost our trusted
       privileges. */
    if (port) {
	/* Place entry into wtmp */
	sprintf(ttyn,"krsh%1d",getpid());
	logwtmp(ttyn,locuser,hostname,1);
    }
    /*      We are simply execing a program over rshd : log entry into wtmp,
	    as kexe(pid), then finish out the session right after that.
	    Syslog should have the information as to what was exec'd */
    else {
	logwtmp(ttyn,locuser,hostname,1);
    }
    
#ifdef CRAY
    
    /* If  we are a secure system then we need to get rid of our
       trusted facility, so that MAC on the chdir we work. Before we
       do this make an entry into wtmp, and any other audit recording. */
    
    if (secflag) {
	if (getusrv(&usrv)){
	    syslog(LOG_ERR,"Cannot getusrv");
	    error("Permission denied.\n");
	    loglogin(hostname, SLG_LVERR, ue->ue_logfails,ue);
	    goto signout_please;
	}
	/*
	 *      6.0 no longer allows any form ofTRUSTED_PROCESS logins.
	 */
	if((ue->ue_valcat & TFM_TRUSTED) ||
	   (sysv.sy_oldtfm &&
	    ((ue->ue_comparts & TRUSTED_SUBJECT) == TRUSTED_SUBJECT))) {
	    loglogin(hostname, SLG_TRSUB, ue->ue_logfails,ue);
	    error("Permission denied.\n");
	    goto signout_please;
	}
	
	loglogin(hostname, SLG_OKLOG, ue->ue_logfails,ue);
	
	/*	Setup usrv structure with user udb info and 
		packet_level and packet_compart. */
	usrv.sv_actlvl = packet_level;
	usrv.sv_actcmp = packet_compart; /*Note get_packet_level sets
					   compartment to users default
					   compartments....*/
	usrv.sv_permit = ue->ue_permits;
	usrv.sv_intcls = ue->ue_intcls;
	usrv.sv_maxcls = ue->ue_maxcls;
	usrv.sv_intcat = ue->ue_intcat;
	usrv.sv_valcat = ue->ue_valcat;
	usrv.sv_savcmp = 0;
	usrv.sv_savlvl = 0;
	
	/*
	 *      Set user values to workstation boundaries
	 */
#ifdef MIN
#undef MIN
#endif
#ifdef MAX
#undef MAX
#endif
	
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
	
	nal_error = 0;
	
	if (nal.na_sort) {
	    if ((ue->ue_minlvl > nal.na_smax) ||
		(ue->ue_maxlvl < nal.na_smin))
	      nal_error++;
	    else {
		usrv.sv_minlvl=MAX(ue->ue_minlvl, nal.na_smin);
		usrv.sv_maxlvl=MIN(ue->ue_maxlvl, nal.na_smax);
		
#ifndef IP_SECURITY

		if (usrv.sv_actlvl < usrv.sv_minlvl)
		    usrv.sv_actlvl = usrv.sv_minlvl;
		if (usrv.sv_actlvl > usrv.sv_maxlvl)
		  usrv.sv_actlvl = usrv.sv_maxlvl;
		
#else /*IP_SECURITY*/
		if (usrv.sv_actlvl < usrv.sv_minlvl)
		  nal_error++;
		if (usrv.sv_actlvl > usrv.sv_maxlvl)
		  nal_error++;
		if (usrv.sv_actlvl != ue->ue_deflvl)
		  nal_error++;
		
		usrv.sv_valcmp = ue->ue_comparts & nal.na_scmp;
		usrv.sv_actcmp &= nal.na_scmp;
#endif /*IP_SECURITY*/
		usrv.sv_valcmp = ue->ue_comparts & nal.na_scmp;
		usrv.sv_actcmp = (usrv.sv_valcmp &
				  ue->ue_defcomps);
	    }
	} else {
	    /*
	     *      If the user's minimum level is greater than
	     *      zero, they cannot log on from this (ie. an
	     *      unclassified) host.
	     */
	    if (ue->ue_minlvl > 0)
	      nal_error++;
	    /*
	      /*
	       *      Address not in NAL, if EXEMPT_NAL is not
	       *      true, then even an unclassified user is
	       *      not allowed.
	       */
	      if (!EXEMPT_NAL)
		nal_error++;
	      else {
		  usrv.sv_minlvl = 0;
		  usrv.sv_maxlvl = 0;
		  usrv.sv_valcmp = 0;
		  usrv.sv_actcmp = 0;
		  usrv.sv_actlvl = 0;
	      }
	}
	if (nal_error) {
	    loglogin(hostname, SLG_LVERR, ue->ue_logfails,ue);
	    error("Permission denied.\n");
	    goto signout_please;
	}
#undef  MIN
#undef  MAX
	/* Before the setusrv is done then do a sethost for paddr */
	sethost(paddr);
	
	if (setusrv(&usrv) == -1) {
	    loglogin(hostname, SLG_LVERR, ue->ue_logfails,ue);
	    error("Permission denied.\n");
	    goto signout_please;
	}
	if (getusrv(&usrv) == -1) {
	    error("Getusrv Permission denied.\n");
	    goto signout_please;
	}
	
    }
#endif /*CRAY*/
    
    if (chdir(pwd->pw_dir) < 0) {
	syslog(LOG_ERR ,
	       "Principal %s  (%s@%s) for local user %s has no home directory.\n",
	       kremuser, remuser, hostname, locuser);
	error("No remote directory.\n");
	goto signout_please;
    }

#ifdef KERBEROS
    if (must_pass_k5 || must_pass_one) {
#if defined(KRB5_KRB4_COMPAT) && !defined(ALWAYS_V5_KUSEROK)
	if (auth_sys == KRB5_RECVAUTH_V4) {
	    /* kuserok returns 0 if OK */
	    if (kuserok(v4_kdata, locuser)){
		syslog(LOG_ERR ,
		       "Principal %s (%s@%s) for local user %s failed kuserok.\n",
		       kremuser, remuser, hostname, locuser);
		if (must_pass_k5) {
		    error("Permission denied.\n");
		    goto signout_please;
		}
		failed_k5 = 1;
	    }
	} else
#endif
	{
	    /* krb5_kuserok returns 1 if OK */
	    if (!krb5_kuserok(bsd_context, client, locuser)){
		syslog(LOG_ERR ,
		       "Principal %s (%s@%s) for local user %s failed krb5_kuserok.\n",
		       kremuser, remuser, hostname, locuser);
		if (must_pass_k5) {
		    error("Permission denied.\n");
		    goto signout_please;
		}
		failed_k5 = 1;
	    }
	}
    }
	
    if (must_pass_rhosts || (failed_k5 && must_pass_one)) {
	/* Cannot check .rhosts unless connection from privileged port */
	if (non_privileged) {
	    syslog(LOG_ERR , "connection from bad port\n");
	    exit(1);
	}

	if (ruserok(hostname, pwd->pw_uid == 0,
		    remuser, locuser) < 0) {
	    syslog(LOG_ERR ,
		   "Principal %s (%s@%s) for local user %s failed ruserok.\n",
		   kremuser, remuser, hostname, locuser);
	    error("Permission denied.\n");
	    goto signout_please;
	}
    }
#else
    if (pwd->pw_passwd != 0 && *pwd->pw_passwd != '\0' &&
	ruserok(hostname, pwd->pw_uid == 0, remuser, locuser) < 0) {
	error("Permission denied.\n");
	goto signout_please;
    }
#endif /* KERBEROS */
    
    if (pwd->pw_uid && !access("/etc/nologin", F_OK)) {
	error("Logins currently disabled.\n");
	goto signout_please;
    }
    
    /* Log access to account */
    pwd = (struct passwd *) getpwnam(locuser);
    if (pwd && (pwd->pw_uid == 0)) {
#ifdef LOG_CMD
	syslog(LOG_NOTICE, "Executing %s for principal %s (%s@%s) as ROOT", 
	       cmdbuf, kremuser, remuser, hostname);
#else
	syslog(LOG_NOTICE ,"Access as ROOT by principal %s (%s@%s)",
	       kremuser, remuser, hostname);
#endif
    }
#if defined(KERBEROS) && defined(LOG_REMOTE_REALM) && !defined(LOG_OTHER_USERS) && !defined(LOG_ALL_LOGINS)
    /* Log if principal is from a remote realm */
    else if (client && !default_realm(client))
#endif
  
#if defined(KERBEROS) && defined(LOG_OTHER_USERS) && !defined(LOG_ALL_LOGINS) 
    /* Log if principal name does not map to local username */
    else if (client && !princ_maps_to_lname(client, locuser))
#endif /* LOG_OTHER_USERS */
  
#ifdef LOG_ALL_LOGINS /* Log everything */
    else 
#endif 
  
#if defined(LOG_REMOTE_REALM) || defined(LOG_OTHER_USERS) || defined(LOG_ALL_LOGINS)
      {
#ifdef LOG_CMD
	  syslog(LOG_NOTICE, "Executing %s for principal %s (%s@%s) as local user %s", 
		 cmdbuf, kremuser, remuser, hostname, locuser);
#else
	  syslog(LOG_NOTICE ,"Access as %s by principal %s (%s@%s)",
		 locuser, kremuser, remuser, hostname);
#endif
      }
#endif
    
    (void) write(2, "", 1);
    
    if (port) {
	if (pipe(pv) < 0) {
	    error("Can't make pipe.\n");
	    goto signout_please;
	}
	if (pipe(pw) < 0) {
	    error("Can't make pipe 2.\n");
	    goto signout_please;
	}
	if (pipe(px) < 0) {
	    error("Can't make pipe 3.\n");
	    goto signout_please;
	}
	pid = fork();
	if (pid == -1)  {
	    error("Fork failed.\n");
	    goto signout_please;
	}
	if (pid) {
#ifdef POSIX_SIGNALS
	    sa.sa_handler = cleanup;
	    (void)sigaction(SIGINT, &sa, (struct sigaction *)0);
	    (void)sigaction(SIGQUIT, &sa, (struct sigaction *)0);
	    (void)sigaction(SIGTERM, &sa, (struct sigaction *)0);
	    (void)sigaction(SIGPIPE, &sa, (struct sigaction *)0);
	    (void)sigaction(SIGHUP, &sa, (struct sigaction *)0);

	    sa.sa_handler = SIG_IGN;
	    (void)sigaction(SIGCHLD, &sa, (struct sigaction *)0);
#else
	    signal(SIGINT, cleanup);
	    signal(SIGQUIT, cleanup);
	    signal(SIGTERM, cleanup);
	    signal(SIGPIPE, cleanup);
	    signal(SIGHUP, cleanup);
	    signal(SIGCHLD,SIG_IGN);
#endif
	    
	    (void) close(0); (void) close(1); (void) close(2);
	    (void) close(pv[1]);
	    (void) close(pw[1]);
	    (void) close(px[0]);
	    
	    ioctl(pv[0], FIONBIO, (char *)&one);
	    ioctl(pw[0], FIONBIO, (char *)&one);
	    /* should set s nbio! */

	    if (do_encrypt)
		if (((*des_write)(s, SECURE_MESSAGE, sizeof(SECURE_MESSAGE))) < 0)
		    fatal(pw[0], "Cannot encrypt-write network.");
	    
	    FD_ZERO(&readfrom);
	    FD_SET(f, &readfrom);
	    FD_SET(s, &readfrom);
	    FD_SET(pv[0], &readfrom);
	    FD_SET(pw[0], &readfrom);
	    
	    do {
		ready = readfrom;
		if (select(8*sizeof(ready), &ready, (fd_set *)0,
			   (fd_set *)0, (struct timeval *)0) < 0)
		  break;
		if (FD_ISSET(s, &ready)) {
		    if ((*des_read)(s, &sig, 1) <= 0)
			FD_CLR(s, &readfrom);
		    else {
#ifdef POSIX_SIGNALS
			sa.sa_handler = cleanup;
			(void)sigaction(sig, &sa, (struct sigaction *)0);
			kill(-pid, sig);
#else
			signal(sig, cleanup);
			killpg(pid, sig);
#endif
		    }
		}
		if (FD_ISSET(f, &ready)) {
		    errno = 0;
		    cc = (*des_read)(f, buf, sizeof(buf));
		    if (cc <= 0) {
			(void) close(px[1]);
			FD_CLR(f, &readfrom);
		    } else
			(void) write(px[1], buf, cc);
		}
		if (FD_ISSET(pv[0], &ready)) {
		    errno = 0;
		    cc = read(pv[0], buf, sizeof (buf));
		    if (cc <= 0) {
			shutdown(s, 1+1);
			FD_CLR(pv[0], &readfrom);
		    } else
			(void) (*des_write)(s, buf, cc);
		}
		if (FD_ISSET(pw[0], &ready)) {
		    errno = 0;
		    cc = read(pw[0], buf, sizeof (buf));
		    if (cc <= 0) {
			shutdown(f, 1+1);
			FD_CLR(pw[0], &readfrom);
		    } else
			(void) (*des_write)(f, buf, cc);
		}
	    } while (FD_ISSET(s, &readfrom) ||
		     FD_ISSET(f, &readfrom) ||
		     FD_ISSET(pv[0], &readfrom) ||
		     FD_ISSET(pw[0], &readfrom));
#ifdef KERBEROS
	    syslog(LOG_INFO ,
		   "Shell process completed.");
#endif
	    /* Finish session in wmtp */
	    logwtmp(ttyn,"","",0);
	    exit(0);
	}
#ifdef SETPGRP_TWOARG
	setpgrp(0, getpid());
#else
	setpgrp();
#endif
	(void) close(s);
	(void) close(f);
	(void) close(pw[0]);
	(void) close(pv[0]);
	(void) close(px[1]);

	(void) dup2(px[0], 0);
	(void) dup2(pw[1], 1);
	(void) dup2(pv[1], 2);

	(void) close(px[0]);
	(void) close(pw[1]);
	(void) close(pv[1]);
    }
    
    /*      We are simply execing a program over rshd : log entry into wtmp, 
	    as kexe(pid), then finish out the session right after that.
	    Syslog should have the information as to what was exec'd */
    else {
	logwtmp(ttyn,"","",0);
    }
    
    if (*pwd->pw_shell == '\0')
      pwd->pw_shell = "/bin/sh";
    (void) close(f);
    (void) setgid((gid_t)pwd->pw_gid);
#ifndef sgi
    initgroups(pwd->pw_name, pwd->pw_gid);
#endif
    (void) setuid((uid_t)pwd->pw_uid);
    environ = envinit;
    strncat(homedir, pwd->pw_dir, sizeof(homedir)-6);
    strncat(shell, pwd->pw_shell, sizeof(shell)-7);
    strncat(username, pwd->pw_name, sizeof(username)-6);
    strcat(path, KPROGDIR);
    strcat(path, ":");
    strcat(path, path_rest);
    cp = strrchr(pwd->pw_shell, '/');
    if (cp)
      cp++;
    else
      cp = pwd->pw_shell;

    if (do_encrypt && !strncmp(cmdbuf, "-x ", 3)) {
	execl(pwd->pw_shell, cp, "-c", (char *)cmdbuf + 3, 0);
    }
    else
	execl(pwd->pw_shell, cp, "-c", cmdbuf, 0);

    perror(pwd->pw_shell);
    perror(cp);
    exit(1);
    
  signout_please:
    logwtmp(ttyn,"","",0);
    exit(1);
}
    


/*VARARGS1*/
error(fmt, a1, a2, a3)
     char *fmt;
     char *a1, *a2, *a3;
{
    char buf[BUFSIZ];
    
    buf[0] = 1;
    (void) sprintf(buf+1, "%s: ", progname);
    (void) sprintf(buf+strlen(buf), fmt, a1, a2, a3);
    (void) write(2, buf, strlen(buf));
    syslog(LOG_ERR ,"%s",buf+1);
}



getstr(fd, buf, cnt, err)
     char *buf;
     int cnt;
     char *err;
{
    char c;
    
    do {
	if (read(fd, &c, 1) != 1)
	  exit(1);
	*buf++ = c;
	if (--cnt == 0) {
	    error("%s too long\n", err);
	    exit(1);
	}
    } while (c != 0);
}



krb5_sigtype 
  cleanup()
{
#ifdef POSIX_SIGNALS
    struct sigaction sa;

    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_IGN;
    (void)sigaction(SIGINT, &sa, (struct sigaction *)0);
    (void)sigaction(SIGQUIT, &sa, (struct sigaction *)0);
    (void)sigaction(SIGTERM, &sa, (struct sigaction *)0);
    (void)sigaction(SIGPIPE, &sa, (struct sigaction *)0);
    (void)sigaction(SIGHUP, &sa, (struct sigaction *)0);

    (void)kill(-pid, SIGTERM);
#else
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    
    killpg(pid, SIGTERM);
#endif
    wait(0);
    
    logwtmp(ttyn,"","",0);
    syslog(LOG_INFO ,"Shell process completed.");
    exit(0);
}



#ifdef	CRAY
char *makejtmp(uid, gid, jid)
     register int uid, gid, jid;
{
    extern int errno;
    
    register char *endc, *tdp = &tmpdir[strlen(tmpdir)];
    register int i;
    
    sprintf(tdp, "%s/jtmp.%06d", JTMPDIR, jid);
    endc = &tmpdir[strlen(tmpdir)];
    
    endc[1] = '\0';
    for (i = 0; i < 26; i++) {
	endc[0] = 'a' + i;
	if (mkdir(tdp, JTMPMODE) != -1) {
	    chown(tdp, uid, gid);
	    return (tdp);
	} else if (errno != EEXIST)
	  break;
    }
    return(NULL);
}



cleanjtmp(user, tpath)
     register char *user, *tpath;
{
    switch(fork()) {
      case -1:
	break;
      case 0:
	if (secflag) {
	    execl("/bin/rm", "rm", "-rf", tpath, 0);
	    error("exec of %s failed; errno = %d\n",
		  "/bin/rm", errno);
	} else {
	    execl(CLEANTMPCMD, CLEANTMPCMD, user, tpath, 0);
	    error("exec of %s failed; errno = %d\n",
		  CLEANTMPCMD, errno);
	}
	exit(1);
	break;
      default:
	/*
	 * Just forget about the child, let init will pick it
	 * up after we exit.
	 */
	break;
    }
}



/***get_packet_classification
 *
 *
 *      int get_packet_classification():
 *      Obtain packet level and compartments from passed fd...
 *
 *      Returns:
 *             -1: If could not get user defaults.
 *              0: success
 */
#ifdef IP_SECURITY
static int get_packet_classification(fd,useruid,level,comp)
     int fd;
     uid_t useruid;
     int *level;
     long *comp;
{
    struct socket_security pkt_sec;
    struct udb *udb;
    int retval;
    int sockoptlen;
    
    retval = 0;
    getsysudb ();
    udb = getudbuid ((int) useruid);
    endudb ();
    if (udb == (struct udb *) 0) return(-1);
    /* Get packet IP packet label */
    sockoptlen = SIZEOF_sec;
    if ( getsockopt(fd,SOL_SOCKET,SO_SECURITY,
		    (char *) &pkt_sec,&sockoptlen)){  /* Failed */
	return(-2);
    }
    *level = pkt_sec.sec_level;
    *comp = udb->ue_defcomps;
    return(0);
}

#else  /* If no IP_SECURITY set level to users default */

static int get_packet_classification(fd,useruid,level,comp)
     int fd;
     uid_t useruid;
     int *level;
     long *comp;
{
    struct udb    *udb;
    getsysudb ();
    udb = getudbuid ((int) useruid);
    endudb ();
    if (udb == (struct udb *) 0) return(-1);
    *level = udb->ue_deflvl;
    *comp = udb->ue_defcomps;
    return(0);
}

#endif /* IP_SECURITY */
	
	

/*
 * Make a security log entry for the login attempt.
 *     host = pointer to host id
 *     flag = status of login
 *     failures = current losing streak in login attempts
 */
/* Make a security log entry for the login attempt.
 *  host = pointer to host id
 *  flag = status of login
 *  failures = current losing streak in login attempts
 */

loglogin(host, flag, failures, ue)
     char    *host;
     int     flag;
     int     failures;
     struct udb * ue;
{
    char   urec[sizeof(struct slghdr) + sizeof(struct slglogin)];
    struct slghdr   *uhdr = (struct slghdr *)urec;
    struct slglogin *ulogin=(struct slglogin *)&urec[sizeof(struct slghdr)];
    
    strncpy(ulogin->sl_line, ttyn, sizeof(ulogin->sl_line));
    strncpy(ulogin->sl_host, host, sizeof(ulogin->sl_host));
    ulogin->sl_failures = failures;
    if ( maxlogs && (failures >= maxlogs))
      flag |= SLG_DSABL;
    ulogin->sl_result = flag;
    uhdr->sl_uid = ue->ue_uid;
    uhdr->sl_ruid = ue->ue_uid;
    uhdr->sl_juid = ue->ue_uid;
    uhdr->sl_gid = ue->ue_gids[0];
    uhdr->sl_rgid = ue->ue_gids[0];
    uhdr->sl_slvl = ue->ue_deflvl;
    /*      uhdr->sl_scls = ue->ue_defcls;  enable for integrity policy */
    uhdr->sl_olvl = 0;
    uhdr->sl_len = sizeof(urec);
    
#ifdef  CRAY2
    slgentry(SLG_LOGN, (word *)urec);
#else /*        ! CRAY2 */
    slgentry(SLG_LOGN, (waddr_t)urec);
#endif
    return;
}

#endif /* CRAY */
	


usage()
{
#ifdef KERBEROS
    syslog(LOG_ERR, "usage: kshd [-rRkK] or [r/R][k/K]shd");
#else
    syslog(LOG_ERR, "usage: rshd");
#endif
}



int princ_maps_to_lname(principal, luser)	
     krb5_principal principal;
     char *luser;
{
    char kuser[10];
    if (!(krb5_aname_to_localname(bsd_context, principal,
				  sizeof(kuser), kuser))
	&& (strcmp(kuser, luser) == 0)) {
	return 1;
    }
    return 0;
}


int default_realm(principal)
     krb5_principal principal;
{
    char *def_realm;
    int realm_length;
    int retval;
    
    realm_length = krb5_princ_realm(bsd_context, principal)->length;
    
    if (retval = krb5_get_default_realm(bsd_context, &def_realm)) {
	return 0;
    }
    
    if ((realm_length != strlen(def_realm)) ||
	(memcmp(def_realm, krb5_princ_realm(bsd_context, principal)->data, 
		realm_length))) {
	free(def_realm);
	return 0;
    }	
    free(def_realm);
    return 1;
}

#ifdef KERBEROS

#ifndef KRB_SENDAUTH_VLEN
#define	KRB_SENDAUTH_VLEN 8	    /* length for version strings */
#endif

#define	KRB_SENDAUTH_VERS	"AUTHV0.1" /* MUST be KRB_SENDAUTH_VLEN
					      chars */

krb5_error_code
recvauth(netf, peersin, peeraddr)
     int netf;
     struct sockaddr_in peersin;
     krb5_address peeraddr;
{
    krb5_error_code status;
    struct sockaddr_in laddr;
    char krb_vers[KRB_SENDAUTH_VLEN + 1];
    int len;
    krb5_principal server;
    krb5_data inbuf;
    char v4_instance[INST_SZ];	/* V4 Instance */
    char v4_version[9];
    krb5_ticket        *ticket;

    len = sizeof(laddr);
    if (getsockname(netf, (struct sockaddr *)&laddr, &len)) {
	    exit(1);
    }
	
#ifdef unicos61
#define SIZEOF_INADDR  SIZEOF_in_addr
#else
#define SIZEOF_INADDR sizeof(struct in_addr)
#endif

    if (status = krb5_sname_to_principal(bsd_context, NULL, "host", 
					 KRB5_NT_SRV_HST, &server)) {
	    syslog(LOG_ERR, "parse server name %s: %s", "host",
		   error_message(status));
	    exit(1);
    }

    strcpy(v4_instance, "*");

    status = krb5_compat_recvauth(bsd_context, &netf,
				  "KCMDV0.1",
				  server, /* Specify daemon principal */
				  &peeraddr, /* We do want to match */
					     /* this against caddrs in */
					     /* the ticket */
				  0, /* use v5srvtab */
				  0, /* no keyproc */
				  0, /* no keyprocarg */
				  0, /* default rc_type */
				  0, /* no flags */

				  0, 		/* v4_opts */
				  "rcmd", 	/* v4_service */
				  v4_instance, 	/* v4_instance */
				  &peersin, 	/* foreign address */
				  &laddr, 	/* our local address */
				  "", 		/* use default srvtab */

				  &auth_sys, /* which authentication system */
				  0, 		/* no seq number */
				  &client, 	/* return client */
				  &ticket, 	/* return ticket */
				  &kdata, 	/* return authenticator */
				  
				  &v4_kdata, 0, v4_version);

    if (status) {
	if (auth_sys == KRB5_RECVAUTH_V5) {
	    /*
	     * clean up before exiting
	     */
	    getstr(netf, locuser, sizeof(locuser), "locuser");
	    getstr(netf, cmdbuf, sizeof(cmdbuf), "command");
	    getstr(netf, remuser, sizeof(locuser), "remuser");
	}
	return status;
    }

    getstr(netf, locuser, sizeof(locuser), "locuser");
    getstr(netf, cmdbuf, sizeof(cmdbuf), "command");

    if (auth_sys == KRB5_RECVAUTH_V4) {
	/* We do not really know the remote user's login name.
         * Assume it to be the same as the first component of the
	 * principal's name. 
         */
	strcpy(remuser, v4_kdata->pname);
	kremuser = (char *) malloc(strlen(v4_kdata->pname) + 1 +
				   strlen(v4_kdata->pinst) + 1 +
				   strlen(v4_kdata->prealm) + 1);
	sprintf(kremuser, "%s/%s@%s", v4_kdata->pname,
		v4_kdata->pinst, v4_kdata->prealm);
	
	if (status = krb5_parse_name(bsd_context, kremuser, &client))
	  return(status);
	return 0;
    }

    /* Must be V5  */
	
    getstr(netf, remuser, sizeof(locuser), "remuser");

    if (status = krb5_unparse_name(bsd_context, client, &kremuser))
	return status;
    
    /* Setup eblock for encrypted sessions. */
    krb5_use_keytype(bsd_context, &eblock, ticket->enc_part2->session->keytype);
    if (status = krb5_process_key(bsd_context, &eblock, ticket->enc_part2->session))
	fatal(netf, "Permission denied");

    /* Null out the "session" because eblock.key references the session
     * key here, and we do not want krb5_free_ticket() to destroy it. */
    ticket->enc_part2->session = 0;

    if (status = krb5_read_message(bsd_context, (krb5_pointer)&netf, &inbuf)) {
	error("Error reading message: %s\n", error_message(status));
	exit(1);
    }

    if (inbuf.length) { /* Forwarding being done, read creds */
	if (status = rd_and_store_for_creds(bsd_context, &inbuf, ticket, locuser)) {
	    error("Can't get forwarded credentials: %s\n",
		  error_message(status));
	    exit(1);
	}
    }
    krb5_free_ticket(bsd_context, ticket);
    return 0;
}


char storage[2*BUFSIZ];                    /* storage for the decryption */
int nstored = 0;
char *store_ptr = storage;

int
v5_des_read(fd, buf, len)
     int fd;
     register char *buf;
     int len;
{
    int nreturned = 0;
    krb5_ui_4 net_len,rd_len;
    int cc,retry;
    unsigned char len_buf[4];
    
    if (!do_encrypt)
      return(read(fd, buf, len));
    
    if (nstored >= len) {
	memcpy(buf, store_ptr, len);
	store_ptr += len;
	nstored -= len;
	return(len);
    } else if (nstored) {
	memcpy(buf, store_ptr, nstored);
	nreturned += nstored;
	buf += nstored;
	len -= nstored;
	nstored = 0;
    }
    
    if ((cc = krb5_net_read(bsd_context, fd, (char *)len_buf, 4)) != 4) {
	if ((cc < 0)  && ((errno == EWOULDBLOCK) || (errno == EAGAIN)))
	    return(cc);
	/* XXX can't read enough, pipe must have closed */
	return(0);
    }
    rd_len =
	((len_buf[0]<<24) |
	 (len_buf[1]<<16) |
	 (len_buf[2]<<8) |
	 len_buf[3]);
    net_len = krb5_encrypt_size(rd_len, eblock.crypto_entry);
    if (net_len < 0 || net_len > sizeof(des_inbuf)) {
	/* XXX preposterous length, probably out of sync.
	   act as if pipe closed */
	syslog(LOG_ERR,"Read size problem (rd_len=%d, net_len=%d)",
	       rd_len, net_len);
	return(0);
    }
    retry = 0;
  datard:
    if ((cc = krb5_net_read(bsd_context, fd, desinbuf.data, net_len)) != net_len) {
	/* XXX can't read enough, pipe must have closed */
	if ((cc < 0)  && ((errno == EWOULDBLOCK) || (errno == EAGAIN))) {
	    retry++;
	    if (retry > MAXRETRIES){
		syslog(LOG_ERR, "des_read retry count exceeded %d\n", retry);
		return(0);
	    }
	    sleep(1);
	    goto datard;
	}
	syslog(LOG_ERR,
	       "Read data received %d != expected %d.",
	       cc, net_len);
	return(0);
    }

    /* decrypt info */
    if (krb5_decrypt(bsd_context, desinbuf.data, (krb5_pointer) storage, net_len,
		     &eblock, 0)) {
	syslog(LOG_ERR,"Read decrypt problem.");
	return(0);
    }

    store_ptr = storage;
    nstored = rd_len;
    if (nstored > len) {
	memcpy(buf, store_ptr, len);
	nreturned += len;
	store_ptr += len;
	nstored -= len;
    } else {
	memcpy(buf, store_ptr, nstored);
	nreturned += nstored;
	nstored = 0;
    }
    return(nreturned);
}
    

int
v5_des_write(fd, buf, len)
     int fd;
     char *buf;
     int len;
{
    unsigned char len_buf[4];
    
    if (!do_encrypt)
      return(write(fd, buf, len));
    
    desoutbuf.length = krb5_encrypt_size(len, eblock.crypto_entry);
    if (desoutbuf.length > sizeof(des_outbuf)){
	syslog(LOG_ERR,"Write size problem (%d > %d)",
	       desoutbuf.length, sizeof(des_outbuf));
	return(-1);
    }

    if (krb5_encrypt(bsd_context, (krb5_pointer)buf, desoutbuf.data, len, &eblock, 0)) {
	syslog(LOG_ERR,"Write encrypt problem.");
	return(-1);
    }
    
    len_buf[0] = (len & 0xff000000) >> 24;
    len_buf[1] = (len & 0xff0000) >> 16;
    len_buf[2] = (len & 0xff00) >> 8;
    len_buf[3] = (len & 0xff);
    (void) write(fd, len_buf, 4);

    if (write(fd, desoutbuf.data, desoutbuf.length) != desoutbuf.length){
	syslog(LOG_ERR,"Could not write out all data.");
	return(-1);
    }
    else return(len);
}


void fatal(f, msg)
     int f;
     char *msg;
{
    char buf[512];
    int out = 1 ;          /* Output queue of f */

    buf[0] = '\01';             /* error indicator */
    (void) sprintf(buf + 1, "%s: %s.\r\n",progname, msg);
    if ((f == netf) && (pid > 0))
      (void) (*des_write)(f, buf, strlen(buf));
    else
      (void) write(f, buf, strlen(buf));
    syslog(LOG_ERR,"%s\n",msg);
    if (pid > 0) {
        signal(SIGCHLD,SIG_IGN);
        kill(pid,SIGKILL);
#ifdef POSIX_TERMIOS
        (void) tcflush(1, TCOFLUSH);
#else
        (void) ioctl(f, TIOCFLUSH, (char *)&out);
#endif
        cleanup();
    }
    exit(1);
}
#endif /* KERBEROS */
