/*									HTTCP.c
**	GENERIC COMMUNICATION CODE
**
**	(c) COPYRIGHT MIT 1995.
**	Please first read the full copyright statement in the file COPYRIGH.
**
**	This code is in common between client and server sides.
**
**	16 Jan 92  TBL	Fix strtol() undefined on CMU Mach.
**	25 Jun 92  JFG  Added DECNET option through TCP socket emulation.
**	13 Sep 93  MD   Added correct return of vmserrorno for HTInetStatus.
**			Added decoding of vms error message for MULTINET.
**	31 May 94  HF	Added cache on host id's; now use inet_ntoa() to
**			HTInetString and some other fixes. Added HTDoConnect
**			and HTDoAccept
*/

/* Library include files */
#include "sysdep.h"
#include "HTUtils.h"
#include "HTString.h"
#include "HTAtom.h"
#include "HTList.h"
#include "HTParse.h"
#include "HTAlert.h"
#include "HTError.h"
#include "HTReqMan.h"
#include "HTNetMan.h"
#include "HTDNS.h"
#include "HTTCP.h"					 /* Implemented here */

/* VMS stuff */
#ifdef VMS
#ifndef MULTINET
#define FD_SETSIZE 32
#else /* Multinet */
#define FD_SETSIZE 256
#endif /* Multinet */
#endif /* VMS */

/* Macros and other defines */
/* x seconds penalty on a multi-homed host if IP-address is down */
#define TCP_PENALTY		1200

/* x seconds penalty on a multi-homed host if IP-address is timed out */
#define TCP_DELAY		600

PRIVATE char *hostname = NULL;			    /* The name of this host */
PRIVATE char *mailaddress = NULL;		     /* Current mail address */

/* ------------------------------------------------------------------------- */

/*
**	Returns the string equivalent to the errno passed in the argument.
**	We can't use errno directly as we have both errno and socerrno. The
**	result is a static buffer.
*/
PUBLIC const char * HTErrnoString (int errornumber)
{
#ifdef HAVE_STRERROR
    return strerror(errornumber);
#else
#ifdef HAVE_SYS_ERRLIST
#ifdef HAVE_SYS_NERR
    return (errno < sys_nerr ? sys_errlist[errno] : "Unknown error");
#else
    return sys_errlist[errno];
#endif /* HAVE_SYS_NERR */
#else
#ifdef VMS
    static char buf[60];
    sprintf(buf, "Unix errno=%ld dec, VMS error=%lx hex", errornumber,
	    vaxc$errno);
    return buf;
#else
#ifdef _WINSOCKAPI_
    static char buf[60];
    sprintf(buf, "Unix errno=%ld dec, WinSock error=%ld", errornumber,
	    WSAGetLastError());
    return buf;
#else
    return "(Error number not translated)";
#endif /* _WINSOCKAPI_ */
#endif /* VMS */
#endif /* HAVE_SYS_ERRLIST */
#endif /* HAVE_STRERROR */
}


/*	Debug error message
*/
PUBLIC int HTInetStatus (int errnum, char * where)
{
#ifdef VMS
    if (PROT_TRACE) HTTrace("System Error Unix = %ld dec\n", errno);
    if (PROT_TRACE) HTTrace("System Error VMS  = %lx hex\n", vaxc$errno);
    return (-vaxc$errno);
#else
#ifdef _WINSOCKAPI_
    if (PROT_TRACE) HTTrace("System Error Unix = %ld dec\n", errno);
    if (PROT_TRACE) HTTrace("System Error WinSock error=%lx hex\n",
			    WSAGetLastError());
    return (-errnum);
#else
    if (PROT_TRACE)
	HTTrace("System Error %d after call to %s() failed\n............ %s\n",
		errno, where, HTErrnoString(errnum));
    return (-errnum);
#endif /* _WINSOCKAPI_ */
#endif /* VMS */
}


/*	Parse a cardinal value				       parse_cardinal()
**	----------------------
**
** On entry,
**	*pp	    points to first character to be interpreted, terminated by
**		    non 0:9 character.
**	*pstatus    points to status already valid
**	maxvalue    gives the largest allowable value.
**
** On exit,
**	*pp	    points to first unread character
**	*pstatus    points to status updated iff bad
*/

PUBLIC unsigned int HTCardinal (int *		pstatus,
				char **		pp,
				unsigned int	max_value)
{
    unsigned int n=0;
    if ( (**pp<'0') || (**pp>'9')) {	    /* Null string is error */
	*pstatus = -3;  /* No number where one expeceted */
	return 0;
    }
    while ((**pp>='0') && (**pp<='9')) n = n*10 + *((*pp)++) - '0';

    if (n>max_value) {
	*pstatus = -4;  /* Cardinal outside range */
	return 0;
    }

    return n;
}

/* ------------------------------------------------------------------------- */
/*	       		        SIGNAL HANDLING 			     */
/* ------------------------------------------------------------------------- */

#ifdef WWWLIB_SIG
/*								    HTSetSignal
**  This function sets up signal handlers. This might not be necessary to
**  call if the application has its own handlers.
*/
#include <signal.h>
PUBLIC void HTSetSignal (void)
{
    /* On some systems (SYSV) it is necessary to catch the SIGPIPE signal
    ** when attemting to connect to a remote host where you normally should
    ** get `connection refused' back
    */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
	if (PROT_TRACE) HTTrace("HTSignal.... Can't catch SIGPIPE\n");
    } else {
	if (PROT_TRACE) HTTrace("HTSignal.... Ignoring SIGPIPE\n");
    }
}
#else
#ifdef WWW_WIN_DLL
PUBLIC void HTSetSignal (void) {}
#endif /* WWW_WIN_DLL */
#endif /* WWWLIB_SIG */

/* ------------------------------------------------------------------------- */
/*	       		     HOST NAME FUNCTIONS 			     */
/* ------------------------------------------------------------------------- */

/*	Produce a string for an Internet address
**	----------------------------------------
**
** On exit,
**	returns	a pointer to a static string which must be copied if
**		it is to be kept.
*/
PUBLIC const char * HTInetString (SockA * sin)
{
#ifndef DECNET  /* Function only used below for a trace message */
#if 0
    /* This dumps core on some Sun systems :-(. The problem is now, that 
       the current implememtation only works for IP-addresses and not in
       other address spaces. */
    return inet_ntoa(sin->sin_addr);
#endif
    static char string[16];
    sprintf(string, "%d.%d.%d.%d",
	    (int)*((unsigned char *)(&sin->sin_addr)+0),
	    (int)*((unsigned char *)(&sin->sin_addr)+1),
	    (int)*((unsigned char *)(&sin->sin_addr)+2),
	    (int)*((unsigned char *)(&sin->sin_addr)+3));
    return string;
#else
    return "";
#endif /* Decnet */
}

/*	Parse a network node address and port
**	-------------------------------------
** 	It is assumed that any portnumber and numeric host address
**	is given in decimal notation. Separation character is '.'
**	Any port number given in host name overrides all other values.
**	'host' might be modified.
**      Returns:
**	       	>0	Number of homes
**		 0	Wait for persistent socket
**		-1	Error
*/
PRIVATE int HTParseInet (HTNet * net, char * host)
{
    int status = 1;
    SockA *sin = &net->sock_addr;

#ifdef DECNET
    /* read Decnet node name. @@ Should know about DECnet addresses, but it's
       probably worth waiting until the Phase transition from IV to V. */

    sin->sdn_nam.n_len = min(DN_MAXNAML, strlen(host));  /* <=6 in phase 4 */
    strncpy (sin->sdn_nam.n_name, host, sin->sdn_nam.n_len + 1);

    if (PROT_TRACE) HTTrace( 
	"DECnet: Parsed address as object number %d on host %.6s...\n",
		      sin->sdn_objnum, host);
#else /* Internet */
    {
	char *strptr = host;
	while (*strptr) {
	    if (*strptr == ':') {
		*strptr = '\0';	   /* Don't want port number in numeric host */
		break;
	    }
	    if (!isdigit(*strptr) && *strptr != '.')
		break;
	    strptr++;
	}
	if (!*strptr) {
#ifdef GUSI
	    sin->sin_addr = inet_addr(host); 		 /* See netinet/in.h */
#else
	    sin->sin_addr.s_addr = inet_addr(host);	  /* See arpa/inet.h */
#endif
	} else
	    status = HTGetHostByName(net, host);

	if (PROT_TRACE) {
	    if (status > 0)
		HTTrace("ParseInet... as port %d on %s with %d homes\n",
			(int) ntohs(sin->sin_port), HTInetString(sin), status);
	}
    }
#endif /* Internet vs. Decnet */
    return status;
}


/*								HTGetDomainName
**	Returns the current domain name without the local host name.
**	The response is pointing to a static area that might be changed
**	using HTSetHostName().
**
**	Returns NULL on error, "" if domain name is not found
*/
PUBLIC const char *HTGetDomainName (void)
{
    const char *host = HTGetHostName();
    char *domain;
    if (host && *host) {
	if ((domain = strchr(host, '.')) != NULL)
	    return ++domain;
	else
	    return "";
    } else
	return NULL;
}


/*								HTSetHostName
**	Sets the current hostname inclusive domain name.
**	If this is not set then the default approach is used using
**	HTGetHostname().
*/
PUBLIC void HTSetHostName (char * host)
{
    if (host && *host) {
	char *strptr;
	StrAllocCopy(hostname, host);
	strptr = hostname;
	while (*strptr) {
	    *strptr = TOLOWER(*strptr);
	    strptr++;
	}
	if (*(hostname+strlen(hostname)-1) == '.')    /* Remove trailing dot */
	    *(hostname+strlen(hostname)-1) = '\0';
    } else {
	if (PROT_TRACE) HTTrace("SetHostName. Bad argument ignored\n");
    }
}


/*								HTGetHostName
**	Returns the name of this host. It uses the following algoritm:
**
**	1) gethostname()
**	2) if the hostname doesn't contain any '.' try to read
**	   /etc/resolv.conf. If there is no domain line in this file then
**	3) Try getdomainname and do as the man pages say for resolv.conf (sun)
**	   If there is no domain line in this file, then it is derived
**	   from the domain name set by the domainname(1) command, usually
**	   by removing the first component. For example, if the domain-
**	   name is set to ``foo.podunk.edu'' then the default domain name
**	   used will be ``pudunk.edu''.
**
**	This is the same procedure as used by res_init() and sendmail.
**
**	Return: hostname on success else NULL
*/
PUBLIC const char * HTGetHostName (void)
{
    int fqdn = 0;				     /* 0=no, 1=host, 2=fqdn */
    FILE *fp;
    char name[MAXHOSTNAMELEN+1];
    if (hostname) {		       			  /* If already done */
	if (*hostname)
	    return hostname;
	else
	    return NULL;		    /* We couldn't get the last time */
    }
    *(name+MAXHOSTNAMELEN) = '\0';

#ifdef HAVE_SYSINFO
    if (!fqdn && sysinfo(SI_HOSTNAME, name, MAXHOSTNAMELEN) > 0) {
	char * dot = strchr(name, '.');
	if (PROT_TRACE) HTTrace("HostName.... sysinfo says `%s\'\n", name);
	StrAllocCopy(hostname, name);
	fqdn = dot ? 2 : 1;
    }
#endif /* HAVE_SYSINFO */

#ifdef HAVE_GETHOSTNAME
    if (!fqdn && gethostname(name, MAXHOSTNAMELEN) == 0) {
	char * dot = strchr(name, '.');
	if (PROT_TRACE) HTTrace("HostName.... gethostname says `%s\'\n", name);
	StrAllocCopy(hostname, name);
	fqdn = dot ? 2 : 1;
    }
#endif /* HAVE_GETHOSTNAME */

#ifdef RESOLV_CONF
    /* Now try the resolver config file */
    if (fqdn==1 && (fp = fopen(RESOLV_CONF, "r")) != NULL) {
	char buffer[80];
	*(buffer+79) = '\0';
	while (fgets(buffer, 79, fp)) {
	    if (!strncasecomp(buffer, "domain", 6)) {	
		char *domainstr = buffer+6;
		char *end;
		while (*domainstr == ' ' || *domainstr == '\t')
		    domainstr++;
		end = domainstr;
		while (*end && !isspace(*end))
		    end++;
		*end = '\0';
		if (*domainstr) {
		    StrAllocCat(hostname, ".");
		    StrAllocCat(hostname, domainstr);
		    fqdn = YES;
		    break;
		}
	    }
	}
	fclose(fp);
    }
#endif /* RESOLV_CONF */

#ifdef HAVE_GETDOMAINNAME
    /* If everything else has failed then try getdomainname */
    if (fqdn==1) {
	if (getdomainname(name, MAXHOSTNAMELEN)) {
	    if (PROT_TRACE)
		HTTrace("HostName.... Can't get domain name\n");
	    StrAllocCopy(hostname, "");
	    return NULL;
	}

	/* If the host name and the first part of the domain name are different
	   then use the former as it is more exact (I guess) */
	if (strncmp(name, hostname, (int) strlen(hostname))) {
	    char *domain = strchr(name, '.');
	    if (!domain)
		domain = name;
	    StrAllocCat(hostname, domain);
	}
    }
#endif /* HAVE_GETDOMAINNAME */

    if (hostname) {
	char *strptr = hostname;
	while (*strptr) {	    
	    *strptr = TOLOWER(*strptr);
	    strptr++;
	}
	if (*(hostname+strlen(hostname)-1) == '.')    /* Remove trailing dot */
	    *(hostname+strlen(hostname)-1) = '\0';
	if (PROT_TRACE) HTTrace("HostName.... FQDN is `%s\'\n", hostname);
    }
    return hostname;
}


/*
**	Free the host name. Called from HTLibTerminate
*/
PUBLIC void HTFreeHostName (void)
{
    HT_FREE(hostname);
}


/*							       HTSetMailAddress
**	Sets the current mail address plus host name and domain name.
**	If this is not set then the default approach is used using
**	HTGetMailAddress(). If the argument is NULL or "" then HTGetMailAddress
**	returns NULL on a succeding request.
*/
PUBLIC void HTSetMailAddress (char * address)
{
    if (!address || !*address)
	StrAllocCopy(mailaddress, "");
    else
	StrAllocCopy(mailaddress, address);
    if (WWWTRACE)
	HTTrace("SetMailAdr.. Set mail address to `%s\'\n",
		mailaddress);
}


/*							       HTGetMailAddress
**
**	Get the mail address of the current user on the current host. The
**	domain name used is the one initialized in HTSetHostName or
**	HTGetHostName. The login name is determined using (ordered):
**
**		getlogin
**		getpwuid(getuid())
**
**	The weakness about the last attempt is if the user has multiple
**	login names each with the same user ID. If this fails as well then:
**
**		LOGNAME environment variable
**		USER environment variable
**
**	Returns NULL if error else pointer to static string
*/
PUBLIC const char * HTGetMailAddress (void)
{
#ifdef HT_REENTRANT
  char name[LOGNAME_MAX+1];    /* For getlogin_r or getUserName */
#endif
#ifdef WWW_MSWINDOWS/* what was the plan for this under windows? - EGP */
  char name[256];    /* For getlogin_r or getUserName */
  unsigned int bufSize = sizeof(name);
#endif
#ifdef HAVE_PWD_H
    struct passwd * pw_info = NULL;
#endif
    char * login = NULL;
    const char * domain;
    if (mailaddress) {
	if (*mailaddress)
	    return mailaddress;
	else
	    return NULL;       /* No luck the last time so we wont try again */
    }

#ifdef WWW_MSWINDOWS
    if (!login && GetUserName(name, &bufSize) != TRUE)
        if (PROT_TRACE) HTTrace("MailAddress. GetUsername returns NO\n");
#endif /* WWW_MSWINDOWS */

#ifdef HAVE_CUSERID
    if (!login && (login = (char *) cuserid(NULL)) == NULL)
        if (PROT_TRACE) HTTrace("MailAddress. cuserid returns NULL\n");
#endif /* HAVE_CUSERID */

#ifdef HAVE_GETLOGIN
#ifdef HT_REENTRANT
    if (!login && (login = (char *) getlogin_r(name, LOGNAME_MAX)) == NULL)
#else
    if (!login && (login = (char *) getlogin()) == NULL)
#endif /* HT_REENTRANT */
	if (PROT_TRACE) HTTrace("MailAddress. getlogin returns NULL\n");
#endif /* HAVE_GETLOGIN */

#ifdef HAVE_PWD_H
    if (!login && (pw_info = getpwuid(getuid())) != NULL)
	login = pw_info->pw_name;
#endif /* HAVE_PWD_H */

    if (!login && (login = getenv("LOGNAME")) == NULL)
	if (PROT_TRACE) HTTrace("MailAddress. LOGNAME not found\n");

    if (!login && (login = getenv("USER")) == NULL)
	if (PROT_TRACE) HTTrace("MailAddress. USER not found\n");

    if (!login) login = HT_DEFAULT_LOGIN;

    if (login) {
	StrAllocCopy(mailaddress, login);
	StrAllocCat(mailaddress, "@");
	if ((domain = HTGetHostName()) != NULL)
	    StrAllocCat(mailaddress, domain);
	else {
	    *mailaddress = '\0';
	    return NULL;			/* Domain name not available */
	}
	return mailaddress;
    }
    return NULL;
}


/*
**	Free the mail address. Called from HTLibTerminate
*/
PUBLIC void HTFreeMailAddress (void)
{
    HT_FREE(mailaddress);
}


/* ------------------------------------------------------------------------- */
/*	       	      CONNECTION ESTABLISHMENT MANAGEMENT 		     */
/* ------------------------------------------------------------------------- */

/*								HTDoConnect()
**
**	Note: Any port indication in URL, e.g., as `host:port' overwrites
**	the default port value.
**
**	returns		HT_ERROR	Error has occured or interrupted
**			HT_OK		if connected
**			HT_WOULD_BLOCK  if operation would have blocked
*/
PUBLIC int HTDoConnect (HTNet * net, char * url, u_short default_port)
{
    int status;
    HTRequest * request = net->request;
    char *fullhost = HTParse(url, "", PARSE_HOST);
    char *at_sign;
    char *host;

    /* if there's an @ then use the stuff after it as a hostname */
    if ((at_sign = strchr(fullhost, '@')) != NULL)
	host = at_sign+1;
    else
	host = fullhost;
    if (!*host) {
	HTRequest_addError(request, ERR_FATAL, NO, HTERR_NO_HOST,
		   NULL, 0, "HTDoConnect");
	HT_FREE(fullhost);
	return HT_ERROR;
    }

    /* Jump into the state machine */
    while (1) {
	switch (net->tcpstate) {
	  case TCP_BEGIN:
	    {
		char *port = strchr(host, ':');
		SockA *sin = &net->sock_addr;
		memset((void *) sin, '\0', sizeof(SockA));
		if (port++ && isdigit(*port)) {
#ifdef DECNET
		    sin->sdn_family = AF_DECnet;
		    sin->sdn_objnum=(unsigned char)(strtol(port,(char**)0,10));
#else
		    sin->sin_family = AF_INET;
		    sin->sin_port = htons(atol(port));
#endif
		} else {
#ifdef DECNET
		    sin->sdn_family = AF_DECnet;
		    net->sock_addr.sdn_objnum = DNP_OBJ;
#else  /* Internet */
		    sin->sin_family = AF_INET;
		    sin->sin_port = htons(default_port);
#endif
		}
	    }
	    if (PROT_TRACE)
		HTTrace("HTDoConnect. Looking up `%s\'\n", host);
	    net->tcpstate = TCP_DNS;
	    break;

	  case TCP_DNS:
	    if ((status = HTParseInet(net, host)) < 0) {
		if (PROT_TRACE)
		    HTTrace("HTDoConnect. Can't locate `%s\'\n", host);
		HTRequest_addError(request, ERR_FATAL, NO,HTERR_NO_REMOTE_HOST,
			   (void *) host, strlen(host), "HTDoConnect");
		net->tcpstate = TCP_ERROR;
		break;
	    }

	    /*
	    ** Wait for a persistent connection. When we return, we check
	    ** that the socket hasn't been closed in the meantime
	    */
	    if (!status) {
		net->tcpstate = TCP_NEED_CONNECT;
		HT_FREE(fullhost);
		HTNet_wait(net);
		return HT_PERSISTENT;
	    }

	    if (!net->retry && status > 1)		/* If multiple homes */
		net->retry = status;
	    if (net->sockfd != INVSOC) {		   /* Reusing socket */
		if (PROT_TRACE)
		    HTTrace("HTDoConnect. REUSING SOCKET %d\n",
			    net->sockfd);
		net->tcpstate = TCP_CONNECTED;
	    } else
		net->tcpstate = TCP_NEED_SOCKET;
	    break;

	  case TCP_NEED_SOCKET:
#ifdef DECNET
	    if ((net->sockfd=socket(AF_DECnet, SOCK_STREAM, 0))==INVSOC)
#else
	    if ((net->sockfd=socket(AF_INET, SOCK_STREAM,IPPROTO_TCP))==INVSOC)
#endif
	    {
		HTRequest_addSystemError(request, ERR_FATAL, socerrno, NO, "socket");
		net->tcpstate = TCP_ERROR;
		break;
	    }
	    if (PROT_TRACE)
		HTTrace("HTDoConnect. Created socket %d\n",net->sockfd);

	    /* If non-blocking protocol then change socket status
	    ** I use fcntl() so that I can ask the status before I set it.
	    ** See W. Richard Stevens (Advan. Prog. in UNIX environment, p.364)
	    ** Be CAREFULL with the old `O_NDELAY' - it will not work as read()
	    ** returns 0 when blocking and NOT -1. FNDELAY is ONLY for BSD and
	    ** does NOT work on SVR4 systems. O_NONBLOCK is POSIX.
	    */
	    if (!net->preemptive) {
#ifdef _WINSOCKAPI_
		{		/* begin windows scope  */
		    HTRequest * rq = request;
		    long levents = FD_READ | FD_WRITE | FD_ACCEPT | 
			FD_CONNECT | FD_CLOSE ;
		    int rv = 0 ;
				    
#ifdef WWW_WIN_ASYNC
		    /* N.B WSAAsyncSelect() turns on non-blocking I/O */
		    rv = WSAAsyncSelect( net->sockfd, rq->hwnd, 
					rq->winMsg, levents);
		    if (rv == SOCKET_ERROR) {
			status = -1 ;
			if (PROT_TRACE) 
			    HTTrace("HTDoConnect. WSAAsyncSelect() fails: %d\n", 
				     WSAGetLastError());
		    } /* error returns */
#else
		    int enable = 1;
		    status = IOCTL(net->sockfd, FIONBIO, &enable);
#endif
		} /* end scope */
#else 
#if defined(VMS)
		{
		    int enable = 1;
		    status = IOCTL(net->sockfd, FIONBIO, &enable);
		}
#else
		if((status = fcntl(net->sockfd, F_GETFL, 0)) != -1) {
#ifdef O_NONBLOCK
		    status |= O_NONBLOCK;			    /* POSIX */
#else
#ifdef F_NDELAY
		    status |= F_NDELAY;				      /* BSD */
#endif /* F_NDELAY */
#endif /* O_NONBLOCK */
		    status = fcntl(net->sockfd, F_SETFL, status);
		}
#endif /* VMS */
#endif /* WINDOW */
		if (PROT_TRACE) {
		    if (status == -1)
			HTTrace("HTDoConnect. Only blocking works\n");
		    else
			HTTrace("HTDoConnect. Non-blocking socket\n");
		}
	    } else if (PROT_TRACE)
		HTTrace("HTDoConnect. Blocking socket\n");

	    /* If multi-homed host then start timer on connection */
	    if (net->retry) net->connecttime = time(NULL);

	    /* Progress */
	    {
		HTAlertCallback *cbf = HTAlert_find(HT_PROG_CONNECT);
		if (cbf)
		    (*cbf)(request,HT_PROG_CONNECT,HT_MSG_NULL,NULL,host,NULL);
	    }
	    net->tcpstate = TCP_NEED_CONNECT;
	    break;

	  case TCP_NEED_CONNECT:
	    status = connect(net->sockfd, (struct sockaddr *) &net->sock_addr,
			     sizeof(net->sock_addr));
	    /*
	     * According to the Sun man page for connect:
	     *     EINPROGRESS         The socket is non-blocking and the  con-
	     *                         nection cannot be completed immediately.
	     *                         It is possible to select(2) for  comple-
	     *                         tion  by  selecting the socket for writ-
	     *                         ing.
	     * According to the Motorola SVR4 man page for connect:
	     *     EAGAIN              The socket is non-blocking and the  con-
	     *                         nection cannot be completed immediately.
	     *                         It is possible to select for  completion
	     *                         by  selecting  the  socket  for writing.
	     *                         However, this is only  possible  if  the
	     *                         socket  STREAMS  module  is  the topmost
	     *                         module on  the  protocol  stack  with  a
	     *                         write  service  procedure.  This will be
	     *                         the normal case.
	     */
#ifdef _WINSOCKAPI_
	    if (status == SOCKET_ERROR)
#else
	    if (status < 0) 
#endif
	    {
#ifdef EAGAIN
		if (socerrno==EINPROGRESS || socerrno==EAGAIN)
#else 
#ifdef _WINSOCKAPI_
		if (socerrno==WSAEWOULDBLOCK)
#else
		if (socerrno==EINPROGRESS)
#endif /* _WINSOCKAPI_ */
#endif /* EAGAIN */
		{
		    if (PROT_TRACE)
			HTTrace("HTDoConnect. WOULD BLOCK `%s'\n",host);
		    HTEvent_Register(net->sockfd, request, (SockOps)FD_CONNECT,
				     net->cbf, net->priority);
		    HT_FREE(fullhost);
		    return HT_WOULD_BLOCK;
		}
		if (socerrno == EISCONN) {
		    net->tcpstate = TCP_CONNECTED;
		    break;
		}
#ifdef _WINSOCKAPI_
		if (socerrno == WSAEBADF)  	       /* We lost the socket */
#else
		if (socerrno == EBADF)  	       /* We lost the socket */
#endif
		{
		    net->tcpstate = TCP_NEED_SOCKET;
		    break;
		}
		if (net->retry) {
		    net->connecttime -= time(NULL);
		    /* Added EINVAL `invalid argument' as this is what I 
		       get back from a non-blocking connect where I should 
		       get `connection refused' on BSD. SVR4 gives SIG_PIPE */
#if defined(__srv4__) || defined (_WINSOCKAPI_)
		    if (socerrno==ECONNREFUSED || socerrno==ETIMEDOUT ||
			socerrno==ENETUNREACH || socerrno==EHOSTUNREACH ||
			socerrno==EHOSTDOWN)
#else
		    if (socerrno==ECONNREFUSED || socerrno==ETIMEDOUT ||
			socerrno==ENETUNREACH || socerrno==EHOSTUNREACH ||
			socerrno==EHOSTDOWN || socerrno==EINVAL)
#endif
		        net->connecttime += TCP_DELAY;
		    else
		        net->connecttime += TCP_PENALTY;
		    HTDNS_updateWeigths(net->dns, net->home, net->connecttime);
		}
		net->tcpstate = TCP_ERROR;		
	    } else
		net->tcpstate = TCP_CONNECTED;
	    break;

	  case TCP_CONNECTED:
	    HTEvent_UnRegister(net->sockfd, (SockOps) FD_CONNECT);
	    if (net->retry) {
		net->connecttime -= time(NULL);
		HTDNS_updateWeigths(net->dns, net->home, net->connecttime);
	    }
	    net->retry = 0;
	    HTChannel_new(net->sockfd, HT_CH_PLAIN, YES);
	    HT_FREE(fullhost);	    
	    net->tcpstate = TCP_BEGIN;
	    return HT_OK;
	    break;

	  case TCP_NEED_BIND:
	  case TCP_NEED_LISTEN:
	  case TCP_ERROR:
	    if (PROT_TRACE) HTTrace("HTDoConnect. Connect failed\n");
	    if (net->sockfd != INVSOC) {
	        HTEvent_UnRegister(net->sockfd, (SockOps) FD_ALL);
		NETCLOSE(net->sockfd);
		net->sockfd = INVSOC;
		if (HTNet_persistent(net)) {		 /* Inherited socket */
		    HTNet_setPersistent(net, NO);
		    net->tcpstate = TCP_NEED_SOCKET;
		    break;
		}
	    }

	    /* Do we have more homes to try? */
	    if (--net->retry > 0) {
	        HTRequest_addSystemError(request, ERR_NON_FATAL, socerrno, NO,
			      "connect");
		net->tcpstate = TCP_DNS;
		break;
	    }
	    HTRequest_addSystemError(request, ERR_FATAL,socerrno,NO,"connect");
	    HTDNS_delete(host);
	    net->retry = 0;
	    HT_FREE(fullhost);
	    net->tcpstate = TCP_BEGIN;
	    return HT_ERROR;
	    break;
	}
    }
}

/*	HTDoAccept()
**	------------
**	This function makes a non-blocking accept which will turn up as ready
**	read in the select.
**	Returns
**		HT_ERROR	Error has occured or interrupted
**		HT_OK		if connected
**		HT_WOULD_BLOCK  if operation would have blocked
*/
PUBLIC int HTDoAccept (HTNet * net)
{
    int status;
    int size = sizeof(net->sock_addr);
    HTRequest *request = net->request;
    if (net->sockfd==INVSOC) {
	if (PROT_TRACE) HTTrace("HTDoAccept.. Invalid socket\n");
	return HT_ERROR;
    }

    /* Progress report */
    {
	HTAlertCallback *cbf = HTAlert_find(HT_PROG_ACCEPT);
	if (cbf) (*cbf)(request, HT_PROG_ACCEPT, HT_MSG_NULL,NULL, NULL, NULL);
    }
    status = accept(net->sockfd, (struct sockaddr *) &net->sock_addr, &size);
#ifdef _WINSOCKAPI_
    if (status == SOCKET_ERROR)
#else
    if (status < 0) 
#endif
    {
#ifdef EAGAIN
	if (socerrno==EINPROGRESS || socerrno==EAGAIN)
#else 
#ifdef _WINSOCKAPI_
        if (socerrno==WSAEWOULDBLOCK)
#else
	if (socerrno==EINPROGRESS)
#endif /* _WINSOCKAPI_ */
#endif /* EAGAIN */
	{
	    if (PROT_TRACE)
		HTTrace("HTDoAccept.. WOULD BLOCK %d\n", net->sockfd);
	    HTEvent_Register(net->sockfd, request, (SockOps) FD_ACCEPT,
			     net->cbf, net->priority);
	    return HT_WOULD_BLOCK;
	}
	HTRequest_addSystemError(request, ERR_WARN, socerrno, YES, "accept");
	if (PROT_TRACE) HTTrace("HTDoAccept.. Accept failed\n");
	if (HTNet_persistent(net)) HTNet_setPersistent(net, NO);
	return HT_ERROR;
    }

    /* Swap to new socket */
    HTEvent_UnRegister(net->sockfd, (SockOps) FD_ACCEPT);
    net->sockfd = status;
    HTChannel_new(net->sockfd, HT_CH_PLAIN, NO);
    if (PROT_TRACE) HTTrace("Accepted.... socket %d\n", status);
    return HT_OK;
}


/*	HTDoListen
**	----------
**	Listens on the specified port. 0 means that we chose it here
**	If master==INVSOC then we listen on all local interfaces (using a 
**	wildcard). If !INVSOC then use this as the local interface
**	returns		HT_ERROR	Error has occured or interrupted
**			HT_OK		if connected
*/
PUBLIC int HTDoListen (HTNet * net, u_short port, SOCKET master, int backlog)
{
    int status;

    /* Jump into the state machine */
    while (1) {
	switch (net->tcpstate) {
	  case TCP_BEGIN:
	    {
		SockA *sin = &net->sock_addr;
		memset((void *) sin, '\0', sizeof(SockA));
#ifdef DECNET
		sin->sdn_family = AF_DECnet;
		sin->sdn_objnum = port;
#else
		sin->sin_family = AF_INET;
		if (master != INVSOC) {
		    int len = sizeof(SockA);
		    if (getsockname(master, (struct sockaddr *) sin, &len)<0) {
			HTRequest_addSystemError(net->request, ERR_FATAL,
						 socerrno, NO, "getsockname");
			net->tcpstate = TCP_ERROR;
			break;
		    }
		} else
		    sin->sin_addr.s_addr = INADDR_ANY;
		sin->sin_port = htons(port);
#endif
	    }
	    if (PROT_TRACE)
		HTTrace("HTDoListen.. Listen on port %d\n", port);
	    net->tcpstate = TCP_NEED_SOCKET;
	    break;

	  case TCP_NEED_SOCKET:
#ifdef DECNET
	    if ((net->sockfd=socket(AF_DECnet, SOCK_STREAM, 0))==INVSOC)
#else
	    if ((net->sockfd=socket(AF_INET, SOCK_STREAM,IPPROTO_TCP))==INVSOC)
#endif
	    {
		HTRequest_addSystemError(net->request, ERR_FATAL, socerrno,
					 NO, "socket");
		net->tcpstate = TCP_ERROR;
		break;
	    }
	    if (PROT_TRACE)
		HTTrace("HTDoListen.. Created socket %d\n",net->sockfd);

	    /* If non-blocking protocol then change socket status
	    ** I use fcntl() so that I can ask the status before I set it.
	    ** See W. Richard Stevens (Advan. Prog. in UNIX environment, p.364)
	    ** Be CAREFULL with the old `O_NDELAY' - it will not work as read()
	    ** returns 0 when blocking and NOT -1. FNDELAY is ONLY for BSD and
	    ** does NOT work on SVR4 systems. O_NONBLOCK is POSIX.
	    */
	    if (!net->preemptive) {
#ifdef _WINSOCKAPI_ 
		{		/* begin windows scope  */
		    long levents = FD_READ | FD_WRITE | FD_ACCEPT | 
			FD_CONNECT | FD_CLOSE ;
		    int rv = 0 ;
				    
#ifdef WWW_WIN_ASYNC
		    /* N.B WSAAsyncSelect() turns on non-blocking I/O */
		    rv = WSAAsyncSelect(net->sockfd, net->request->hwnd, 
					net->request->winMsg, levents);
		    if (rv == SOCKET_ERROR) {
			status = -1 ;
			if (PROT_TRACE) 
			    HTTrace("HTDoListen.. WSAAsyncSelect() fails: %d\n", 
				     WSAGetLastError());
			} /* error returns */
#else
		    int enable = 1 ;
		    status = IOCTL(net->sockfd, FIONBIO, &enable);
#endif
		} /* end scope */
#else 
#if defined(VMS)
		{
		    int enable = 1;
		    status = IOCTL(net->sockfd, FIONBIO, &enable);
		}
#else
		if((status = fcntl(net->sockfd, F_GETFL, 0)) != -1) {
#ifdef O_NONBLOCK
		    status |= O_NONBLOCK;			    /* POSIX */
#else
#ifdef F_NDELAY
		    status |= F_NDELAY;				      /* BSD */
#endif /* F_NDELAY */
#endif /* O_NONBLOCK */
		    status = fcntl(net->sockfd, F_SETFL, status);
		}
#endif /* VMS */
#endif /* WINDOW */
		if (PROT_TRACE) {
		    if (status == -1)
			HTTrace("HTDoListen.. Blocking socket\n");
		    else
			HTTrace("HTDoListen.. Non-blocking socket\n");
		}
	    }
	    net->tcpstate = TCP_NEED_BIND;
	    break;

	  case TCP_NEED_BIND:
	    status = bind(net->sockfd, (struct sockaddr *) &net->sock_addr,
			  sizeof(net->sock_addr));
#ifdef _WINSOCKAPI_
	    if (status == SOCKET_ERROR)
#else
	    if (status < 0) 
#endif
	    {
		if (PROT_TRACE)
		    HTTrace("Bind........ failed %d\n", socerrno);
		net->tcpstate = TCP_ERROR;		
	    } else
		net->tcpstate = TCP_NEED_LISTEN;
	    break;

	  case TCP_NEED_LISTEN:
	    status = listen(net->sockfd, backlog);
#ifdef _WINSOCKAPI_
	    if (status == SOCKET_ERROR)
#else
	    if (status < 0) 
#endif
		net->tcpstate = TCP_ERROR;		
	    else
		net->tcpstate = TCP_CONNECTED;
	    break;

	  case TCP_CONNECTED:
	    net->tcpstate = TCP_BEGIN;
	    if (PROT_TRACE)
		HTTrace("HTDoListen.. Bind and listen on port %d %s\n",
			(int) ntohs(net->sock_addr.sin_port),
			HTInetString(&net->sock_addr));
	    return HT_OK;
	    break;

	  case TCP_NEED_CONNECT:
	  case TCP_DNS:
	  case TCP_ERROR:
	    if (PROT_TRACE) HTTrace("HTDoListen.. Listen failed\n");
	    HTRequest_addSystemError(net->request, ERR_FATAL, socerrno, NO, "HTDoListen");
	    net->tcpstate = TCP_BEGIN;
	    return HT_ERROR;
	    break;
	}
    }
}

