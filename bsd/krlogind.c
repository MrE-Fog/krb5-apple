/*
 *	$Source$
 *	$Header$
 */


#ifndef lint
static char *rcsid_rlogind_c = "$Header$";
#endif	/* lint */

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

#ifndef lint
static char sccsid[] = "@(#)rlogind.c	5.17 (Berkeley) 8/31/88";
#endif /* not lint */
     
     /*
      * remote login server:
      *	remuser\0
      *	locuser\0
      *	terminal info\0
      *	data
      */
     
/*
 * This is the rlogin daemon. The very basic protocol for checking 
 * authentication and authorization is:
 * 1) Check authentication.
 * 2) Check authorization via the access-control files: 
 *    ~/.k5login (using krb5_kuserok) and/or
 *    ~/.rhosts  (using ruserok).
 * 3) Prompt for password if any checks fail, or if so configured.
 * Allow login if all goes well either by calling the accompanying 
 * login.krb5 or /bin/login, according to the definition of 
 * DO_NOT_USE_K_LOGIN.
 * 
 * The configuration is done either by command-line arguments passed by 
 * inetd, or by the name of the daemon. If command-line arguments are
 * present, they  take priority. The options are:
 * -k and -K means check .k5login (using krb5_kuserok).
 * -r and -R means check .rhosts  (using ruserok).
 * -p and -P means prompt for password.
 * The difference between upper and lower case is as follows:
 *    If lower case -r or -k, then as long as one of krb5_kuserok or 
 * ruserok passes, allow login without password. If the -p option is
 * passed with -r or -k, then if both checks fail, allow login but
 * only after password verification. 
 *    If uppercase -R or -K, then those checks must be passed,
 * regardless of other checks, else no login with or without password.
 *    If the -P option is passed, then the password is verified in 
 * addition to all other checks. If -p is not passed with -k or -r,
 * and both checks fail, then login permission is denied.
 *    -x and -e means use encryption.
 *
 *    If no command-line arguments are present, then the presence of the 
 * letters kKrRexpP in the program-name before "logind" determine the 
 * behaviour of the program exactly as with the command-line arguments.
 *
 * If the ruserok check is to be used, then the client should connect
 * from a privileged port, else deny permission.
 */ 
     
/* DEFINES:
 *   KERBEROS - Define this if application is to be kerberised.
 *   CRYPT    - Define this if encryption is to be an option.
 *   DO_NOT_USE_K_LOGIN - Define this if you want to use /bin/login
 *              instead  of the accompanying login.krb5. In that case,
 *              the remote user's name must be present in the local
 *              .rhosts file, regardless of any options specified.
 *   SERVE_V4 - Define this if v4 rlogin clients are also to be served.
 *   ALWAYS_V5_KUSEROK - Define this if you want .k5login to be
 *              checked even for v4 clients (instead of .klogin).
 *   LOG_ALL_LOGINS - Define this if you want to log all logins.
 *   LOG_OTHER_USERS - Define this if you want to log all principals
 *              that do not map onto the local user.
 *   LOG_REMOTE_REALM - Define this if you want to log all principals from 
 *              remote realms.
 *       Note:  Root logins are always logged.
 */

/*
 * This is usually done in the Makefile.  Actually, these sources may
 * not work without the KERBEROS #defined.
 *
 * #define KERBEROS
 */
#define LOG_REMOTE_REALM
#define CRYPT

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <ctype.h>

/* #include <sys/unistd.h>  ??? What system has a sys/unistd.h? */
     
#include <netinet/in.h>
#include <errno.h>
#include <pwd.h>
     
#ifdef sun
#include <sys/label.h>
#include <sys/audit.h>
#include <pwdadj.h>
#endif
     
#include <signal.h>
#ifdef hpux
#include <sys/ptyio.h>
#endif
     
#ifdef sysvimp
#include <compat.h>
#define STREAMS
#include <sys/stropts.h>
#endif
     
#ifdef SYSV
#define USE_TERMIO
#endif
     
#ifdef USE_TERMIO
#include <termio.h>
#else
#include <sgtty.h>
#endif /* USE_TERMIO */
     
#include <netdb.h>
#include <syslog.h>
#include <strings.h>
#include <sys/param.h>
#include <utmp.h>
     
#ifdef NO_WINSIZE
struct winsize {
    unsigned short ws_row, ws_col;
    unsigned short ws_xpixel, ws_ypixel;
};
#endif /* NO_WINSIZE */
     
#ifdef KERBEROS
     
#include <krb5/krb5.h>
#include <krb5/osconf.h>
#include <krb5/asn1.h>
#include <krb5/crc-32.h>
#include <krb5/mit-des.h>
#include <krb5/los-proto.h>
#include <kerberosIV/krb.h>

#ifdef BUFSIZ
#undef BUFSIZ
#endif

#undef KRB5_KRB4_COMPAT

int auth_sys = 0;	/* Which version of Kerberos used to authenticate */

#define KRB5_RECVAUTH_V4	4
#define KRB5_RECVAUTH_V5	5

int non_privileged = 0; /* set when connection is seen to be from */
			/* a non-privileged port */

AUTH_DAT	*v4_kdata;
Key_schedule v4_schedule;
int v4_des_read(), v4_des_write();

#define BUFSIZ 5120

int v5_des_read(), v5_des_write();

#include <com_err.h>
     
#define SECURE_MESSAGE  "This rlogin session is using DES encryption for all data transmissions.\r\n"

int (*des_read)(), (*des_write)();
char des_inbuf[2*BUFSIZ];         /* needs to be > largest read size */
char des_outbuf[2*BUFSIZ];        /* needs to be > largest write size */
krb5_data desinbuf,desoutbuf;
krb5_encrypt_block eblock;        /* eblock for encrypt/decrypt */

krb5_authenticator      *kdata;
krb5_ticket     *ticket = 0;

#ifdef CRAY
#ifndef BITS64
#define BITS64
#endif
#endif

#define ARGSTR	"rRkKeExXpPD:?"
#else /* !KERBEROS */
#define ARGSTR	"rRpPD:?"
#define (*des_read)  read
#define (*des_write) write
#endif /* KERBEROS */

#ifndef LOGIN_PROGRAM
#ifdef DO_NOT_USE_K_LOGIN
#ifdef sysvimp
#define LOGIN_PROGRAM "/bin/remlogin"
#else
#define LOGIN_PROGRAM "/bin/login"
#endif
#else /* DO_NOT_USE_K_LOGIN */
#define LOGIN_PROGRAM KRB5_PATH_LOGIN
#endif /* DO_NOT_USE_K_LOGIN */
#endif /* LOGIN_PROGRAM */

struct utmp	wtmp;
#define MAXRETRIES 4
#define	UT_NAMESIZE	sizeof(((struct utmp *)0)->ut_name)
#define MAX_PROG_NAME 16

char		lusername[UT_NAMESIZE+1];
char		rusername[UT_NAMESIZE+1];
char            *krusername = 0;
char		term[64];
char            rhost_name[128];
krb5_principal  client;

extern	int errno;
int	reapchild();
struct	passwd *getpwnam();
#if (!defined(__STDC__) && !defined(ultrix))
char	*malloc();
#endif
char 	*progname;

static	int Pfd;

#if (defined(_AIX) && defined(i386)) || defined(ibm032) || (defined(vax) && !defined(ultrix)) || (defined(SunOS) && SunOS > 40) || defined(solaris20)
#define VHANG_FIRST
#endif

#if defined(ultrix)
#define VHANG_LAST		/* vhangup must occur on close, not open */
#endif

void	fatal(), fatalperror(), doit(), usage(), do_krb_login();
int	princ_maps_to_lname(), default_realm();

int must_pass_rhosts = 0, must_pass_k5 = 0, must_pass_one = 0;
int do_encrypt = 0, passwd_if_fail = 0, passwd_req = 0;

main(argc, argv)
     int argc;
     char **argv;
{
    extern int opterr, optind;
    extern char * optarg;
    int on = 1, fromlen, ch, i;
    struct sockaddr_in from;
    char *options;
    int debug_port = 0;
    int fd;
    
    progname = *argv;
    
#ifdef KERBEROS
    
#ifndef LOG_NDELAY
#define LOG_NDELAY 0
#endif
    
    
#ifndef LOG_AUTH /* 4.2 syslog */
    openlog(progname, LOG_PID|LOG_NDELAY);
#else
    openlog(progname, LOG_PID | LOG_AUTH | LOG_NDELAY, LOG_AUTH);
#endif /* 4.2 syslog */
    
#else /* ! KERBEROS */
    
#ifndef LOG_AUTH /* 4.2 syslog */
    openlog("rlogind", LOG_PID| LOG_NDELAY);
#else
    openlog("rlogind", LOG_PID | LOG_AUTH | LOG_NDELAY, LOG_AUTH);
#endif /* 4.2 syslog */
    
#endif /* KERBEROS */
    
    if (argc == 1) { /* Get parameters from program name. */
	if (strlen(progname) > MAX_PROG_NAME) {
	    usage();
	    exit(1);
	}
	options = (char *) malloc(MAX_PROG_NAME+1);
	options[0] = '\0';
	for (i = 0; (progname[i] != '\0') && (i < MAX_PROG_NAME); i++)
	  if (!strcmp(progname+i, "logind")) {
	      char **newargv;

	      newargv = (char **) malloc(sizeof(char *) * 3);

	      strcpy(options, "-");
	      strncat(options, progname, i);

	      argc = 2;
	      
	      newargv[0] = argv[0];
	      newargv[1] = options;
	      newargv[2] = NULL;

	      argv = newargv;
	      break;
	  }
	if (options[0] == '\0') {
	    usage();
	    exit(1);
	}
    }
    
    /* Analyse parameters. */
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
#ifdef CRYPT
	case 'x':         /* Use encryption. */
	case 'X':
	case 'e':
	case 'E':
	  do_encrypt = 1;
	  break;
#endif
#endif
	case 'p':
	  passwd_if_fail = 1; /* Passwd reqd if any check fails */
	  break;
	case 'P':         /* passwd is a must */
	  passwd_req = 1;
	  break;
	case 'D':
	  debug_port = atoi(optarg);
	  break;
	case '?':
	default:
	  usage();
	  exit(1);
	  break;
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
     } 
     else {
	 if (getpeername(0, (struct sockaddr *)&from, &fromlen) < 0) {
	     syslog(LOG_ERR,"Can't get peer name of remote host: %m");
#ifdef STDERR_FILENO
	     fatal(STDERR_FILENO, "Can't get peer name of remote host", 1);
#else
	     fatal(3, "Can't get peer name of remote host", 1);
#endif
	 }
	 fd = 0;
     }

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof (on)) < 0)
      syslog(LOG_WARNING, "setsockopt (SO_KEEPALIVE): %m");
    
    doit(fd, &from);
}



#ifndef LOG_AUTH
#define LOG_AUTH 0
#endif

int	child;
int	cleanup();
int	netf;
char	line[MAXPATHLEN];
extern	char	*inet_ntoa();

#ifdef TIOCSWINSZ
struct winsize win = { 0, 0, 0, 0 };
#endif

int pid; /* child process id */

void doit(f, fromp)
     int f;
     struct sockaddr_in *fromp;
{
    int i, p, t, vfd, on = 1;
    register struct hostent *hp;
    char c;
    char buferror[255];
    struct passwd *pwd;
    
    netf = -1;
    alarm(60);
    read(f, &c, 1);
    
    if (c != 0){
	exit(1);
    }
    
    alarm(0);
    fromp->sin_port = ntohs((u_short)fromp->sin_port);
    hp = gethostbyaddr(&fromp->sin_addr, sizeof (struct in_addr),
		       fromp->sin_family);
    if (hp == 0) {
	/*
	 * Only the name is used below.
	 */
	sprintf(rhost_name,"%s",inet_ntoa(fromp->sin_addr));
    }
    
    /* Save hostent information.... */
    else strcpy(rhost_name,hp->h_name);
    
    if (fromp->sin_family != AF_INET)
      fatal(f, "Permission denied - Malformed from address\n");
    
#ifdef KERBEROS
    if (must_pass_k5 || must_pass_one) {
	/* Init error messages and setup des buffers */
	krb5_init_ets();
	desinbuf.data = des_inbuf;
	desoutbuf.data = des_outbuf;    /* Set up des buffers */
    }
    /* Must come from privileged port when .rhosts is being looked into */
    if ((must_pass_rhosts || must_pass_one) 
	&& (fromp->sin_port >= IPPORT_RESERVED ||
	    fromp->sin_port < IPPORT_RESERVED/2))
      non_privileged = 1;
#else /* !KERBEROS */
    if (fromp->sin_port >= IPPORT_RESERVED ||
	fromp->sin_port < IPPORT_RESERVED/2)
      fatal(f, "Permission denied - Connection from bad port");
#endif /* KERBEROS */
    
    /* Set global netf to f now : we may need to drop everything
       in do_krb_login. */
    netf = f;
    
#if defined(KERBEROS)
    /* All validation, and authorization goes through do_krb_login() */
    do_krb_login(rhost_name);
#else
    getstr(f, rusername, sizeof(rusername), "remuser");
    getstr(f, lusername, sizeof(lusername), "locuser");
    getstr(f, term, sizeof(term), "Terminal type");
#endif
    
    write(f, "", 1);
    if (getpty(&p,line))
      fatal(f, "Out of ptys");
    Pfd = p;
#ifdef TIOCSWINSZ
    (void) ioctl(p, TIOCSWINSZ, &win);
#endif
    
#ifndef sysvimp  /* IMP has a problem with opening and closing
		    it's stream pty by the parent process */
    
    /* Make sure we can open slave pty, then close it for system 5 so that 
       the process group is set correctly..... */
#ifdef VHANG_FIRST
    vfd = open(line, O_RDWR);
    if (vfd < 0)
      fatalperror(f, line);
#ifdef NOFCHMOD
    if (chmod(vfd,0))
#else
    if (fchmod(vfd, 0))
#endif
      fatalperror(f, line);
#ifndef SYSV
    if (f == 0) {  /* if operating standalone, do not reset tty!! */
	signal(SIGHUP, SIG_IGN);
	vhangup();
	signal(SIGHUP, SIG_DFL);
    }
#endif 
#endif /* VHANG_FIRST */
#if defined (sun) || defined (POSIX)
    setsid();
#endif

    t = open(line, O_RDWR);
#ifdef VHANG_FIRST
#ifndef VHANG_NO_CLOSE
	(void) close(vfd);
#endif
#endif /* VHANG_FIRST */
    if (t < 0)
      fatalperror(f, line);
#ifdef SYSV
    close(t);
#endif
#endif  /* sysvimp */
    signal(SIGCHLD, cleanup);
    signal(SIGTERM, cleanup);
    pid = fork();
    if (pid < 0)
      fatalperror(f, "", errno);
    if (pid == 0) {
	{
#ifdef USE_TERMIO
	    struct termio b;
#define TIOCGETP TCGETA
#define TIOCSETP TCSETA
#ifdef MIN
#undef MIN
#endif
#define        MIN     1
#define        TIME    0
	    
#else
	    struct sgttyb b;
#endif
#ifdef SYSV
	    (void) setpgrp();
	    /* SYSV open slave device: We closed it above so pgrp
	       would be set correctly...*/
	    t = open(line, O_RDWR);
	    if (t < 0)
	      fatalperror(f, line);
#endif
#ifdef STREAMS
	    while (ioctl (t, I_POP, 0) == 0); /*Clear out any old lined's*/
#endif
	    /* Under Ultrix 3.0, the pgrp of the slave pty terminal
	       needs to be set explicitly.  Why rlogind works at all
	       without this on 4.3BSD is a mystery.
	       It seems to work fine on 4.3BSD with this code enabled.
	       IMP's need both ioctl and setpgrp..
	       */
#if !defined(SYSV) || defined(sysvimp)
	    /* SYSV set process group prior to opening pty */
#ifdef sysvimp
	    pid = 0;
#else
#ifdef convex
	    pid = getpgrp();
#else
	    pid = getpgrp(getpid());
#endif
#endif
#ifdef POSIX /* solaris */
	    /* we've already done setsid above. Just do tcsetpgrp here. */
	    tcsetpgrp(0, pid);
#else
#ifndef hpux
	    ioctl(0, TIOCSPGRP, &pid);
#else
	    /* we've already done setsid above. Just do tcsetpgrp here. */
	    tcsetpgrp(0, pid);
#endif
#endif /* posix */
	    pid = 0;			/*reset pid incase exec fails*/
#endif
#ifdef STREAMS
	    if (line_push(t) < 0)
	      fatalperror(f, "IPUSH",errno);
#endif
	    (void)ioctl(t, TIOCGETP, &b);
#ifdef USE_TERMIO
	    /* The key here is to just turn off echo */
	    b.c_iflag &= ~(ICRNL|IUCLC);
	    b.c_iflag |= IXON;
	    b.c_cflag |= CS8;
	    b.c_lflag |= ICANON|ISIG;
	    b.c_lflag &= ~(ECHO);
	    b.c_cc[VMIN] = MIN;
	    b.c_cc[VTIME] = TIME;
#else
	    b.sg_flags = RAW|ANYP;
#endif
	    (void)ioctl(t, TIOCSETP, &b);
	    /*
	     **      signal the parent that we have turned off echo
	     **      on the slave side of the pty ... he's waiting
	     **      because otherwise the rlogin protocol junk gets
	     **      echo'd to the user (locuser^@remuser^@term/baud)
	     **      and we don't get the right tty affiliation, and
	     **      other kinds of hell breaks loose ...
	     */
	    (void) write(t, &c, 1);
	    
	}
	close(f), close(p);
	dup2(t, 0), dup2(t, 1), dup2(t, 2);
	if (t > 2)
	  close(t);
#if defined(sysvimp)
	setcompat (COMPAT_CLRPGROUP | (getcompat() & ~COMPAT_BSDTTY));
#endif
	
	/* Log access to account */
	pwd = (struct passwd *) getpwnam(lusername);
	if (pwd && (pwd->pw_uid == 0)) {
	    if (passwd_req)
	      syslog(LOG_NOTICE, "ROOT login by %s (%s@%s) forcing password access",
		     krusername ? krusername : "", rusername, rhost_name);
	    else
	      syslog(LOG_NOTICE, "ROOT login by %s (%s@%s) ", 
		     krusername ? krusername : "", rusername, rhost_name);
	}
	
#if defined(KERBEROS) && defined(LOG_REMOTE_REALM) && !defined(LOG_OTHER_USERS) && !defined(LOG_ALL_LOGINS)
	/* Log if principal is from a remote realm */
        else if (client && !default_realm(client))
#endif
  
#if defined(KERBEROS) && defined(LOG_OTHER_USERS) && !defined(LOG_ALL_LOGINS) 
	/* Log if principal name does not map to local username */
        else if (client && !princ_maps_to_lname(client, lusername))
#endif /* LOG_OTHER_USERS */
  
#ifdef LOG_ALL_LOGINS /* Log everything */
	else 
#endif
  
#if defined(LOG_REMOTE_REALM) || defined(LOG_OTHER_USERS) || defined(LOG_ALL_LOGINS)
        {
	    if (passwd_req)
	      syslog(LOG_NOTICE,
		     "login by %s (%s@%s) as %s forcing password access\n",
		     krusername ? krusername : "", rusername,
		     rhost_name, lusername);
	    else 
	      syslog(LOG_NOTICE,
		     "login by %s (%s@%s) as %s\n",
		     krusername ? krusername : "", rusername,
		     rhost_name, lusername); 
	}
#endif
	
#ifdef DO_NOT_USE_K_LOGIN
	execl(LOGIN_PROGRAM, "login", "-r", rhost_name, 0);
#else
	if (passwd_req)
	  execl(LOGIN_PROGRAM, "login","-h", rhost_name, lusername, 0);
	else
	  execl(LOGIN_PROGRAM, "login", "-h", rhost_name, "-e", lusername, 0);
#endif
	
	fatalperror(2, LOGIN_PROGRAM, errno);
	/*NOTREACHED*/
    }
    /*
     **      wait for child to start ... read one byte
     **      -- see the child, who writes one byte after
     **      turning off echo on the slave side ...
     **      The master blocks here until it reads a byte.
     */
    if (read(p, &c, 1) != 1) {
	/*
	 * Problems read failed ...
	 */
	sprintf(buferror, "Cannot read slave pty %s ",line);
	fatalperror(p,buferror,errno);
    }
    
#if defined(KERBEROS) 
    if (do_encrypt) {
	if (((*des_write)(f, SECURE_MESSAGE, sizeof(SECURE_MESSAGE))) < 0){
	    sprintf(buferror, "Cannot encrypt-write network.");
	    fatal(p,buferror);
	}
    }
    else 
      /*
       * if encrypting, don't turn on NBIO, else the read/write routines
       * will fail to work properly
       */
#endif /* KERBEROS */
      ioctl(f, FIONBIO, &on);
    ioctl(p, FIONBIO, &on);
#ifdef hpux
    /******** FIONBIO doesn't currently work on ptys, should be O_NDELAY? **/
    /*** get flags and add O_NDELAY **/
    (void) fcntl(p,F_SETFL,fcntl(p,F_GETFL,0) | O_NDELAY);
#endif
    
    ioctl(p, TIOCPKT, &on);
    signal(SIGTSTP, SIG_IGN);
#ifdef hpux
    setpgrp2(0, 0);
#else
    setpgrp(0, 0);
#endif
    
#ifdef DO_NOT_USE_K_LOGIN
    /* Pass down rusername and lusername to login. */
    (void) write(p, rusername, strlen(rusername) +1);
    (void) write(p, lusername, strlen(lusername) +1);
#endif
    /* stuff term info down to login */
    if( write(p, term, strlen(term)+1) <= 0 ){
	/*
	 * Problems write failed ...
	 */
	sprintf(buferror,"Cannot write slave pty %s ",line);
	fatalperror(f,buferror,errno);
    } 
    
    protocol(f, p);
    signal(SIGCHLD, SIG_IGN);
    cleanup();
}

unsigned char	magic[2] = { 0377, 0377 };
#ifdef TIOCSWINSZ
#ifndef TIOCPKT_WINDOW
#define TIOCPKT_WINDOW 0x80
#endif
unsigned char	oobdata[] = {TIOCPKT_WINDOW};
#else
char    oobdata[] = {0};
#endif

/*
 * Handle a "control" request (signaled by magic being present)
 * in the data stream.  For now, we are only willing to handle
 * window size changes.
 */
control(pty, cp, n)
     int pty;
     char *cp;
     int n;
{
    struct winsize w;
    int pgrp;
    
    if (n < 4+sizeof (w) || cp[2] != 's' || cp[3] != 's')
      return (0);
#ifdef TIOCSWINSZ
    oobdata[0] &= ~TIOCPKT_WINDOW;	/* we know he heard */
    memcpy((char *)&w,cp+4, sizeof(w));
    w.ws_row = ntohs(w.ws_row);
    w.ws_col = ntohs(w.ws_col);
    w.ws_xpixel = ntohs(w.ws_xpixel);
    w.ws_ypixel = ntohs(w.ws_ypixel);
    (void)ioctl(pty, TIOCSWINSZ, &w);
    if (ioctl(pty, TIOCGPGRP, &pgrp) >= 0)
      (void) killpg(pgrp, SIGWINCH);
#endif
    return (4+sizeof (w));
}



/*
 * rlogin "protocol" machine.
 */
protocol(f, p)
     int f, p;
{
    char pibuf[1024], fibuf[1024], *pbp, *fbp;
    register pcc = 0, fcc = 0;
    int cc;
    char cntl;
    
    /*
     * Must ignore SIGTTOU, otherwise we'll stop
     * when we try and set slave pty's window shape
     * (our controlling tty is the master pty).
     */
    signal(SIGTTOU, SIG_IGN);
#ifdef TIOCSWINSZ
    send(f, oobdata, 1, MSG_OOB);	/* indicate new rlogin */
#endif
    for (;;) {
	int ibits, obits, ebits;
	
	ibits = 0;
	obits = 0;
	if (fcc)
	  obits |= (1<<p);
	else
	  ibits |= (1<<f);
	if (pcc >= 0)
	  if (pcc)
	    obits |= (1<<f);
	  else
	    ibits |= (1<<p);
	ebits = (1<<p);
	if (select(16, &ibits, &obits, &ebits, 0) < 0) {
	    if (errno == EINTR)
	      continue;
	    fatalperror(f, "select");
	}
	if (ibits == 0 && obits == 0 && ebits == 0) {
	    /* shouldn't happen... */
	    sleep(5);
	    continue;
	}
#define	pkcontrol(c)	((c)&(TIOCPKT_FLUSHWRITE|TIOCPKT_NOSTOP|TIOCPKT_DOSTOP))
	if (ebits & (1<<p)) {
	    cc = read(p, &cntl, 1);
	    if (cc == 1 && pkcontrol(cntl)) {
		cntl |= oobdata[0];
		send(f, &cntl, 1, MSG_OOB);
		if (cntl & TIOCPKT_FLUSHWRITE) {
		    pcc = 0;
		    ibits &= ~(1<<p);
		}
	    }
	}
	if (ibits & (1<<f)) {
	    fcc = (*des_read)(f, fibuf, sizeof (fibuf));
	    if (fcc < 0 && errno == EWOULDBLOCK)
	      fcc = 0;
	    else {
		register char *cp;
		int left, n;
		
		if (fcc <= 0)
		  break;
		fbp = fibuf;
		
	      top:
		for (cp = fibuf; cp < fibuf+fcc-1; cp++)
		  if (cp[0] == magic[0] &&
		      cp[1] == magic[1]) {
		      left = fcc - (cp-fibuf);
		      n = control(p, cp, left);
		      if (n) {
			  left -= n;
			  if (left > 0)
			    memcpy(cp,
				   cp+n,
				   left);
			  fcc -= n;
			  goto top; /* n^2 */
		      }
		  }
	    }
	}
	
	if ((obits & (1<<p)) && fcc > 0) {
	    cc = write(p, fbp, fcc);
	    if (cc > 0) {
		fcc -= cc;
		fbp += cc;
	    }
	}
	
	if (ibits & (1<<p)) {
	    pcc = read(p, pibuf, sizeof (pibuf));
	    pbp = pibuf;
	    if (pcc < 0 && errno == EWOULDBLOCK)
	      pcc = 0;
	    else if (pcc <= 0)
	      break;
	    else if (pibuf[0] == 0)
	      pbp++, pcc--;
	    else {
		if (pkcontrol(pibuf[0])) {
		    pibuf[0] |= oobdata[0];
		    send(f, &pibuf[0], 1, MSG_OOB);
		}
		pcc = 0;
	    }
	}
	if ((obits & (1<<f)) && pcc > 0) {
	    cc = (*des_write)(f, pbp, pcc);
	    if (cc < 0 && errno == EWOULDBLOCK) {
		/* also shouldn't happen */
		sleep(5);
		continue;
	    }
	    if (cc > 0) {
		pcc -= cc;
		pbp += cc;
	    }
	}
    }
}



int cleanup()
{
    char *p;
    
    /* 
      I dont know why P starts with the character '/', but apparently it
      has to do with the way login set line when the initial entry for this
      line is made.
      */
    p = line + sizeof("/dev/") -1 ;
    if (!logout(p)) {
#ifdef SYSV
	logwtmp(p, "", "", 0, 0);
#else
	logwtmp(p, "", "", 0);
#endif
    }
    else 
      syslog(LOG_ERR ,
	     "Cannot delete entry from utmp for %s\n",p);
    
    (void)chmod(line, 0666);
    (void)chown(line, 0, 0);
#ifndef STREAMS
    *p = 'p';
    (void)chmod(line, 0666);
    (void)chown(line, 0, 0);
#endif
#ifdef VHANG_LAST
    close(Pfd);
    vhangup();
#endif
    shutdown(netf, 2);
    exit(1);
}


void fatal(f, msg)
     int f;
     char *msg;
{
    char buf[512];
    int out = 1 ;          /* Output queue of f */
    
    buf[0] = '\01';		/* error indicator */
    (void) sprintf(buf + 1, "%s: %s.\r\n",progname, msg);
    if ((f == netf) && (pid > 0))
      (void) (*des_write)(f, buf, strlen(buf));
    else
      (void) write(f, buf, strlen(buf));
    syslog(LOG_ERR,"%s\n",msg);
    if (pid > 0) {
	signal(SIGCHLD,SIG_IGN);
	kill(pid,SIGKILL);
#ifdef  TIOCFLUSH
	(void) ioctl(f, TIOCFLUSH, (char *)&out);
#else
	(void) ioctl(f, TCFLSH, out);
#endif
	cleanup();
    }
    exit(1);
}



void fatalperror(f, msg)
     int f;
     char *msg;
{
    char buf[512];
    extern int sys_nerr;
    extern char *sys_errlist[];
    
    if ((unsigned)errno < sys_nerr)
      (void) sprintf(buf, "%s: %s", msg, sys_errlist[errno]);
    else
      (void) sprintf(buf, "%s: Error %d", msg, errno);
    fatal(f, buf);
}

#ifdef KERBEROS

void
do_krb_login(host)
     char *host;
{
    krb5_error_code status;
    struct passwd *pwd;
    int	passed_krb, passed_rhosts;
    char *msg_fail;

    passed_krb = passed_rhosts = 0;

    if (getuid()) {
	exit(1);
    }

    /* Check authentication. This can be either Kerberos V5, */
    /* Kerberos V4, or host-based. */
    if (status = recvauth()) {
	if (ticket)
	  krb5_free_ticket(ticket);
	if (status != 255)
	  syslog(LOG_ERR,
		 "Authentication failed from %s: %s\n",
		 host,error_message(status));
	fatal(netf, "Kerberos authentication failed");
	return;
    }
    
    /* OK we have authenticated this user - now check authorization. */
    /* The Kerberos authenticated programs must use krb5_kuserok or kuserok*/
    
#ifndef KRB5_KRB4_COMPAT
    if (auth_sys == KRB5_RECVAUTH_V4) {
	  fatal(netf, "This server does not support Kerberos V4");
  }
#endif
    
    if (must_pass_k5 || must_pass_one) {
#if (defined(ALWAYS_V5_KUSEROK) || !defined(KRB5_KRB4_COMPAT))
	/* krb5_kuserok returns 1 if OK */
	if (client && krb5_kuserok(client, lusername))
	    passed_krb++;
#else
	if (auth_sys == KRB5_RECVAUTH_V4) {
	    /* kuserok returns 0 if OK */
	    if (!kuserok(v4_kdata, lusername))
		passed_krb++;
	} else {
	    /* krb5_kuserok returns 1 if OK */
	    if (client && krb5_kuserok(client, lusername))
		passed_krb++;
	}
#endif
    }
    
    /*  The kerberos authenticated request must pass ruserok also
	if asked for. */
    
    if (!must_pass_k5 &&
	(must_pass_rhosts || (!passed_krb && must_pass_one))) {
	/* Cannot check .rhosts unless connection from a privileged port. */
	if (non_privileged) 
	  fatal(netf, "Permission denied - Connection from bad port");

	pwd = (struct passwd *) getpwnam(lusername);
	if (pwd &&
	    !ruserok(rhost_name, pwd->pw_uid == 0, rusername, lusername))
	    passed_rhosts++;
    }

    if ((must_pass_k5 && passed_krb) ||
	(must_pass_rhosts && passed_rhosts) ||
	(must_pass_one && (passed_krb || passed_rhosts)))
	    return;
    
    if (ticket)
	krb5_free_ticket(ticket);

    msg_fail =  (char *) malloc( strlen(krusername) + strlen(lusername) + 80 );
    if (!msg_fail)
	fatal(netf, "User is not authorized to login to specified account");
    sprintf(msg_fail, "User %s is not authorized to login to account %s",
	    krusername, lusername);
    fatal(netf, msg_fail);
    /* NOTREACHED */
}



getstr(fd, buf, cnt, err)
     int fd;
     char *buf;
     int cnt;
     char *err;
{
    
    char c;
    
    do {
	if (read(fd, &c, 1) != 1) {
	    exit(1);
	}
	if (--cnt < 0) {
	    printf("%s too long\r\n", err);
	    exit(1);
	}
	*buf++ = c;
    } while (c != 0);
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
    long net_len,rd_len;
    int cc,retry;
    
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
    
#ifdef BITS64
    rd_len = 0;
    if ((cc = krb5_net_read(fd, (char *)&rd_len + 4, 4)) != 4) {
#else	
    if ((cc = krb5_net_read(fd, (char *)&rd_len, sizeof(rd_len))) !=
	sizeof(rd_len)) {
#endif
	if ((cc < 0)  && (errno == EWOULDBLOCK)) return(cc);
	/* XXX can't read enough, pipe
	   must have closed */
	return(0);
    }
    rd_len = ntohl(rd_len);
    net_len = krb5_encrypt_size(rd_len,eblock.crypto_entry);
    if (net_len < 0 || net_len > sizeof(des_inbuf)) {
	/* XXX preposterous length, probably out of sync.
	   act as if pipe closed */
	syslog(LOG_ERR,"Read size problem.");
	return(0);
    }
    retry = 0;
  datard:
    if ((cc = krb5_net_read(fd, desinbuf.data, net_len)) != net_len) {
	/* XXX can't read enough, pipe
	   must have closed */
	if ((cc < 0)  && (errno == EWOULDBLOCK)) {
	    retry++;
	    sleep(1);
	    if (retry > MAXRETRIES){
		syslog(LOG_ERR,
		       "des_read retry count exceeded %d\n",
		       retry);
		return(0);
	    }
	    goto datard;
	}
	syslog(LOG_ERR,
	       "Read data received %d != expected %d.",
	       cc, net_len);
	return(0);
    }
    /* decrypt info */
    if ((krb5_decrypt(desinbuf.data,
		      (krb5_pointer) storage,
		      net_len,
		      &eblock, 0))) {
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
    long net_len;
    
    if (!do_encrypt)
      return(write(fd, buf, len));
    
    
    desoutbuf.length = krb5_encrypt_size(len,eblock.crypto_entry);
    if (desoutbuf.length > sizeof(des_outbuf)){
	syslog(LOG_ERR,"Write size problem.");
	return(-1);
    }
    if ((krb5_encrypt((krb5_pointer)buf,
		      desoutbuf.data,
		      len,
		      &eblock,
		      0))){
	syslog(LOG_ERR,"Write encrypt problem.");
	return(-1);
    }
    
    net_len = htonl(len);	
#ifdef BITS64
    (void) write(fd,(char *)&net_len + 4, 4);
#else
    (void) write(fd, &net_len, sizeof(net_len));
#endif
    if (write(fd, desoutbuf.data,desoutbuf.length) != desoutbuf.length){
	syslog(LOG_ERR,"Could not write out all data.");
	return(-1);
    }
    else return(len);
}
#endif /* KERBEROS */



getpty(fd,slave)
     int *fd;
     char *slave;
{
    char c;
    int i,ptynum;
    struct stat stb;
#ifdef STREAMS
#ifdef sysvimp
    *fd = open("/dev/pty", O_RDWR|O_NDELAY);
#else
    *fd = open("/dev/ptc", O_RDWR|O_NDELAY);
#endif
    if (*fd >= 0) {
	if (fstat(*fd, &stb) < 0) {
	    close(*fd);
	    return 1;
	}
	ptynum = (int)(stb.st_rdev&0xFF);
#ifdef sysvimp
	sprintf(slave, "/dev/ttyp%x", ptynum);
#else
	sprintf(slave, "/dev/ttyq%x", ptynum);
#endif
    }
    return (0);
    
#else /* NOT STREAMS */
    for (c = 'p'; c <= 's'; c++) {
	sprintf(slave,"/dev/ptyXX");
	slave[strlen("/dev/pty")] = c;
	slave[strlen("/dev/ptyp")] = '0';
	if (stat(slave, &stb) < 0)
	  break;
	for (i = 0; i < 16; i++) {
	    slave[sizeof("/dev/ptyp") - 1] = "0123456789abcdef"[i];
	    *fd = open(slave, O_RDWR);
	    if (*fd > 0)
	      goto gotpty;
	}
    }
    return(1);
  gotpty:
    slave[strlen("/dev/")] = 't';
    return(0);
#endif /* STREAMS */
}



void usage()
{
#ifdef KERBEROS
    syslog(LOG_ERR, 
	   "usage: klogind [-rRkKxpP] [-D port] or [r/R][k/K][x/e][p/P]logind");
#else
    syslog(LOG_ERR, 
	   "usage: rlogind [-rRpP] [-D port] or [r/R][p/P]logind");
#endif
}



#ifdef KERBEROS
int princ_maps_to_lname(principal, luser)	
     krb5_principal principal;
     char *luser;
{
    char kuser[10];
    if (!(krb5_aname_to_localname(principal,
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
    
    realm_length = krb5_princ_realm(principal)->length;
    
    if (retval = krb5_get_default_realm(&def_realm)) {
	return 0;
    }
    
    if ((realm_length != strlen(def_realm)) ||
	(memcmp(def_realm, krb5_princ_realm(principal)->data, realm_length))) {
	free(def_realm);
	return 0;
    }	
    free(def_realm);
    return 1;
}


#ifndef KRB_SENDAUTH_VLEN
#define	KRB_SENDAUTH_VLEN 8	    /* length for version strings */
#endif

#define	KRB_SENDAUTH_VERS	"AUTHV0.1" /* MUST be KRB_SENDAUTH_VLEN
					      chars */

krb5_error_code
recvauth()
{
    krb5_error_code status;
    struct sockaddr_in peersin, laddr;
    char krb_vers[KRB_SENDAUTH_VLEN + 1];
    int len;
    krb5_principal server;
    krb5_address peeraddr;
    krb5_data inbuf;
    char v4_instance[INST_SZ];	/* V4 Instance */
    char v4_version[9];

    len = sizeof(laddr);
    if (getsockname(netf, (struct sockaddr *)&laddr, &len)) {
	    exit(1);
    }
	
    len = sizeof(peersin);
    if (getpeername(netf, (struct sockaddr *)&peersin, &len)) {
	syslog(LOG_ERR, "get peer name failed %d", netf);
	exit(1);
    }

#ifdef unicos61
#define SIZEOF_INADDR  SIZEOF_in_addr
#else
#define SIZEOF_INADDR sizeof(struct in_addr)
#endif

    peeraddr.addrtype = peersin.sin_family;
    peeraddr.length = SIZEOF_INADDR;
    peeraddr.contents = (krb5_octet *)&peersin.sin_addr;
	
    if (status = krb5_sname_to_principal(NULL, "host", KRB5_NT_SRV_HST,
					 &server)) {
	    syslog(LOG_ERR, "parse server name %s: %s", "host",
		   error_message(status));
	    exit(1);
    }

    strcpy(v4_instance, "*");

    status = krb5_compat_recvauth(&netf,
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

				  do_encrypt ? KOPT_DO_MUTUAL : 0, /*v4_opts*/
				  "rcmd", /* v4_service */
				  v4_instance, /* v4_instance */
				  &peersin, /* foriegn address */
				  &laddr, /* our local address */
				  "", /* use default srvtab */

				  &auth_sys, /* which authentication system */
				  0, /* no seq number */
				  &client, /* return client */
				  &ticket, /* return ticket */
				  &kdata, /* return authenticator */
				  
				  &v4_kdata, v4_schedule, v4_version);

    if (status) {
	if (auth_sys == KRB5_RECVAUTH_V5) {
	    /*
	     * clean up before exiting
	     */
	    getstr(netf, lusername, sizeof (lusername), "locuser");
	    getstr(netf, term, sizeof(term), "Terminal type");
	    getstr(netf, rusername, sizeof(rusername), "remuser");
	}
	return status;
    }

    getstr(netf, lusername, sizeof (lusername), "locuser");
    getstr(netf, term, sizeof(term), "Terminal type");

#ifdef KRB5_KRB4_COMPAT
    if (auth_sys == KRB5_RECVAUTH_V4) {

	des_read  = v4_des_read;
	des_write = v4_des_write;

	/* We do not really know the remote user's login name.
         * Assume it to be the same as the first component of the
	 * principal's name. 
         */
	strcpy(rusername, v4_kdata->pname);
	krusername = (char *) malloc(strlen(v4_kdata->pname) + 1 +
				     strlen(v4_kdata->pinst) + 1 +
				     strlen(v4_kdata->prealm) + 1);
	sprintf(krusername, "%s/%s@%s", v4_kdata->pname,
		v4_kdata->pinst, v4_kdata->prealm);
	
	if (status = krb5_parse_name(krusername, &client))
	  return(status);
	return 0;
    }
#endif

    /* Must be V5  */
	
    des_read  = v5_des_read;
    des_write = v5_des_write;

    getstr(netf, rusername, sizeof(rusername), "remuser");

    if (status = krb5_unparse_name(client, &krusername))
	return status;
    
    /* Setup up eblock if encrypted login session */
    /* otherwise zero out session key */
    if (do_encrypt) {
	krb5_use_keytype(&eblock,
			 ticket->enc_part2->session->keytype);
	if (status = krb5_process_key(&eblock,
				      ticket->enc_part2->session))
	    fatal(netf, "Permission denied");
    }      

    if (status = krb5_read_message((krb5_pointer)&netf, &inbuf))
	fatal(netf, "Error reading message");

    if (inbuf.length) { /* Forwarding being done, read creds */
	if (status = rd_and_store_for_creds(&inbuf, ticket, lusername))
	    fatal(netf, "Can't get forwarded credentials");
    }
    return 0;
}


#ifdef KRB5_KRB4_COMPAT

int
v4_des_read(fd, buf, len)
int fd;
register char *buf;
int len;
{
	int nreturned = 0;
	long net_len, rd_len;
	int cc;

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
	
	if ((cc = krb_net_read(fd, &net_len, sizeof(net_len))) != sizeof(net_len)) {
		/* XXX can't read enough, pipe
		   must have closed */
		return(0);
	}
	net_len = ntohl(net_len);
	if (net_len < 0 || net_len > sizeof(des_inbuf)) {
		/* XXX preposterous length, probably out of sync.
		   act as if pipe closed */
		return(0);
	}
	/* the writer tells us how much real data we are getting, but
	   we need to read the pad bytes (8-byte boundary) */
	rd_len = roundup(net_len, 8);
	if ((cc = krb_net_read(fd, des_inbuf, rd_len)) != rd_len) {
		/* XXX can't read enough, pipe
		   must have closed */
		return(0);
	}
	(void) pcbc_encrypt(des_inbuf,
			    storage,
			    (net_len < 8) ? 8 : net_len,
			    v4_schedule,
			    v4_kdata->session,
			    DECRYPT);
	/* 
	 * when the cleartext block is < 8 bytes, it is "right-justified"
	 * in the block, so we need to adjust the pointer to the data
	 */
	if (net_len < 8)
		store_ptr = storage + 8 - net_len;
	else
		store_ptr = storage;
	nstored = net_len;
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
v4_des_write(fd, buf, len)
int fd;
char *buf;
int len;
{
	long net_len;
	static int seeded = 0;
	static char garbage_buf[8];
	long garbage;

	if (!do_encrypt)
		return(write(fd, buf, len));

	/* 
	 * pcbc_encrypt outputs in 8-byte (64 bit) increments
	 *
	 * it zero-fills the cleartext to 8-byte padding,
	 * so if we have cleartext of < 8 bytes, we want
	 * to insert random garbage before it so that the ciphertext
	 * differs for each transmission of the same cleartext.
	 * if len < 8 - sizeof(long), sizeof(long) bytes of random
	 * garbage should be sufficient; leave the rest as-is in the buffer.
	 * if len > 8 - sizeof(long), just garbage fill the rest.
	 */

#ifdef min
#undef min
#endif
#define min(a,b) ((a < b) ? a : b)

	if (len < 8) {
		if (!seeded) {
			seeded = 1;
			srandom((int) time((long *)0));
		}
		garbage = random();
		/* insert random garbage */
		(void) memcpy(garbage_buf, &garbage, min(sizeof(long),8));

		/* this "right-justifies" the data in the buffer */
		(void) memcpy(garbage_buf + 8 - len, buf, len);
	}
	(void) pcbc_encrypt((len < 8) ? garbage_buf : buf,
			    des_outbuf,
			    (len < 8) ? 8 : len,
			    v4_schedule,
			    v4_kdata->session,
			    ENCRYPT);

	/* tell the other end the real amount, but send an 8-byte padded
	   packet */
	net_len = htonl(len);
	(void) write(fd, &net_len, sizeof(net_len));
	(void) write(fd, des_outbuf, roundup(len,8));
	return(len);
}

#endif /* KRB5_KRB4_COMPAT */
#endif /* KERBEROS */
