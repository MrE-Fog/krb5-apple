/* here are actual path values from each operating system supported. */
/* LPATH is from rlogin, for login.c; RPATH is from rsh, for rshd.c */
#ifdef sun
#ifdef solaris20
#define RPATH ":/usr/bin"
#define LPATH "/usr/bin:"
#else
/* sun3 and sun4 */
#define LPATH ":/usr/ucb:/bin:/usr/bin"
#define RPATH ":/usr/ucb:/bin:/usr/bin"
#endif
#endif

#ifdef ultrix
#define LPATH ":/usr/ucb:/bin:/usr/bin"
#define RPATH ":/usr/ucb:/bin:/usr/bin"
#endif

#ifdef hpux
/* hpux 8, both hppa and s300 */
#define LPATH "/bin:/usr/bin:/usr/contrib/bin:/usr/local/bin"
#define RPATH "/bin:/usr/bin:/usr/contrib/bin:/usr/local/bin"
#endif

#ifdef NeXT
#define LPATH ":/usr/ucb:/bin:/usr/bin:/usr/local/bin"
#define RPATH "/bin:/usr/ucb:/usr/bin:"
#endif

#ifdef _IBMR2
/* 3.2.0 */ 
#define LPATH "/usr/bin:/etc:/usr/sbin:/usr/ucb:/usr/bin/X11:/sbin"
#define RPATH ":/usr/ucb:/bin:/usr/bin:/usr/bin/X11"
#endif

#ifdef __SCO__
#define LPATH "/bin:/usr/bin:/usr/dbin:/usr/ldbin"
#define RPATH "/bin:/usr/bin:/usr/local/bin::"
#endif

#ifdef sgi
#define LPATH ":/usr/sbin:/usr/bsd:/usr/bin:/bin:/usr/bin/X11"
#define RPATH ":/usr/sbin:/usr/bsd:/usr/bin:/bin:/usr/bin/X11"
#endif

#ifdef linux
#define LPATH "/local/bin:/usr/bin:/bin:/usr/local/bin:/usr/bin/X11:."
#define RPATH "/local/bin:/usr/bin:/bin:/usr/local/bin:/usr/bin/X11:."
#endif

#ifdef __386BSD__
#define LPATH "/usr/bin:/bin"
#define RPATH "/usr/bin:/bin"
#endif

#ifdef __alpha
#ifdef __osf__
#define LPATH "/usr/bin:."
#define RPATH "/usr/bin:/bin:"
#endif
#endif

#ifdef __pyrsoft
#ifdef MIPSEB
#define RPATH ":/bin:/usr/bin"
#define LPATH "/usr/bin:/usr/ccs/bin:/usr/ucb:."
#endif
#endif

#ifdef __DGUX
#ifdef __m88k__
#define RPATH "/usr/bin"
#define LPATH "/usr/bin"
#endif
#endif

#ifndef LPATH
#ifdef __svr4__
/* taken from unixware, sirius... */
#define RPATH ":/bin:/usr/bin:/usr/X/bin"
#define LPATH "/usr/bin:/usr/dbin:/usr/dbin"
#endif
#endif

#ifndef LPATH
#ifdef __NetBSD__
#define LPATH "/usr/bin:/bin"
#define RPATH "/usr/bin:/bin"
#endif
#endif
