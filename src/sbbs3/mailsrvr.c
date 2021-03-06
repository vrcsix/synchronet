/* mailsrvr.c */

/* Synchronet Mail (SMTP/POP3) server and sendmail threads */

/* $Id: mailsrvr.c,v 1.563 2013/02/11 22:52:13 deuce Exp $ */

/****************************************************************************
 * @format.tab-size 4		(Plain Text/Source Code File Header)			*
 * @format.use-tabs true	(see http://www.synchro.net/ptsc_hdr.html)		*
 *																			*
 * Copyright 2012 Rob Swindell - http://www.synchro.net/copyright.html		*
 *																			*
 * This program is free software; you can redistribute it and/or			*
 * modify it under the terms of the GNU General Public License				*
 * as published by the Free Software Foundation; either version 2			*
 * of the License, or (at your option) any later version.					*
 * See the GNU General Public License for more details: gpl.txt or			*
 * http://www.fsf.org/copyleft/gpl.html										*
 *																			*
 * Anonymous FTP access to the most recent released source is available at	*
 * ftp://vert.synchro.net, ftp://cvs.synchro.net and ftp://ftp.synchro.net	*
 *																			*
 * Anonymous CVS access to the development source and modification history	*
 * is available at cvs.synchro.net:/cvsroot/sbbs, example:					*
 * cvs -d :pserver:anonymous@cvs.synchro.net:/cvsroot/sbbs login			*
 *     (just hit return, no password is necessary)							*
 * cvs -d :pserver:anonymous@cvs.synchro.net:/cvsroot/sbbs checkout src		*
 *																			*
 * For Synchronet coding style and modification guidelines, see				*
 * http://www.synchro.net/source.html										*
 *																			*
 * You are encouraged to submit any modifications (preferably in Unix diff	*
 * format) via e-mail to mods@synchro.net									*
 *																			*
 * Note: If this box doesn't appear square, then you need to fix your tabs.	*
 ****************************************************************************/

/* ANSI C Library headers */
#include <limits.h>			/* UINT_MAX */
#include <stdio.h>
#include <stdlib.h>			/* ltoa in GNU C lib */
#include <stdarg.h>			/* va_list */
#include <string.h>			/* strrchr */
#include <ctype.h>			/* isdigit */
#include <fcntl.h>			/* Open flags */
#include <errno.h>			/* errno */

/* Synchronet-specific headers */
#undef SBBS	/* this shouldn't be defined unless building sbbs.dll/libsbbs.so */
#include "sbbs.h"
#include "mailsrvr.h"
#include "mime.h"
#include "md5.h"
#include "crc32.h"
#include "base64.h"
#include "ini_file.h"
#include "netwrap.h"	/* getNameServerList() */
#include "xpendian.h"
#include "js_rtpool.h"
#include "js_request.h"

/* Constants */
static const char*	server_name="Synchronet Mail Server";
#define FORWARD			"forward:"
#define NO_FORWARD		"local:"

int dns_getmx(char* name, char* mx, char* mx2
			  ,DWORD intf, DWORD ip_addr, BOOL use_tcp, int timeout);

static char* pop_err	=	"-ERR";
static char* ok_rsp		=	"250 OK";
static char* auth_ok	=	"235 User Authenticated";
static char* sys_error	=	"421 System error";
static char* sys_unavail=	"421 System unavailable, try again later";
static char* insuf_stor =	"452 Insufficient system storage";
static char* badarg_rsp =	"501 Bad argument";
static char* badseq_rsp	=	"503 Bad sequence of commands";
static char* badauth_rsp=	"535 Authentication failure";
static char* badrsp_err	=	"%s replied with:\r\n\"%s\"\r\n"
							"instead of the expected reply:\r\n\"%s ...\"";

#define TIMEOUT_THREAD_WAIT		60		/* Seconds */
#define DNSBL_THROTTLE_VALUE	1000	/* Milliseconds */
#define SPAM_HASH_SUBJECT_MIN_LEN	10	/* characters */

#define STATUS_WFC	"Listening"

static mail_startup_t* startup=NULL;
static scfg_t	scfg;
static SOCKET	server_socket=INVALID_SOCKET;
static SOCKET	submission_socket=INVALID_SOCKET;
static SOCKET	pop3_socket=INVALID_SOCKET;
static protected_uint32_t active_clients;
static protected_uint32_t thread_count;
static volatile int		active_sendmail=0;
static volatile BOOL	sendmail_running=FALSE;
static volatile BOOL	terminate_server=FALSE;
static volatile BOOL	terminate_sendmail=FALSE;
static sem_t	sendmail_wakeup_sem;
static char		revision[16];
static volatile time_t	uptime;
static str_list_t recycle_semfiles;
static str_list_t shutdown_semfiles;
static int		mailproc_count;
static js_server_props_t js_server_props;

struct {
	volatile ulong	sockets;
	volatile ulong	errors;
	volatile ulong	crit_errors;
	volatile ulong	connections_ignored;
	volatile ulong	connections_refused;
	volatile ulong	connections_served;
	volatile ulong	pop3_served;
	volatile ulong	smtp_served;
	/* SMTP: */
	volatile ulong	sessions_refused;
	volatile ulong	msgs_ignored;
	volatile ulong	msgs_refused;
	volatile ulong	msgs_received;
} stats;

struct mailproc {
	char		name[INI_MAX_VALUE_LEN];
	char		cmdline[INI_MAX_VALUE_LEN];
	char		eval[INI_MAX_VALUE_LEN];
	str_list_t	to;
	str_list_t	from;
	BOOL		passthru;
	BOOL		native;
	BOOL		ignore_on_error;	/* Ignore mail message if cmdline fails */
	BOOL		disabled;
	BOOL		process_spam;
	BOOL		process_dnsbl;
	uint8_t*	ar;
	ulong		handled;			/* counter (for stats display) */
} *mailproc_list;

typedef struct {
	SOCKET			socket;
	SOCKADDR_IN		client_addr;
} smtp_t,pop3_t;

static int lprintf(int level, const char *fmt, ...)
{
	va_list argptr;
	char sbuf[1024];

	va_start(argptr,fmt);
    vsnprintf(sbuf,sizeof(sbuf),fmt,argptr);
	sbuf[sizeof(sbuf)-1]=0;
    va_end(argptr);

	if(level <= LOG_ERR) {
		errorlog(&scfg,startup==NULL ? NULL:startup->host_name,sbuf), stats.errors++;
		if(startup!=NULL && startup->errormsg!=NULL)
			startup->errormsg(startup->cbdata,level,sbuf);
	}

	if(level <= LOG_CRIT)
		stats.crit_errors++;

    if(startup==NULL || startup->lputs==NULL || level > startup->log_level)
		return(0);

#if defined(_WIN32)
	if(IsBadCodePtr((FARPROC)startup->lputs))
		return(0);
#endif

    return(startup->lputs(startup->cbdata,level,sbuf));
}

#ifdef _WINSOCKAPI_

static WSADATA WSAData;
#define SOCKLIB_DESC WSAData.szDescription
static BOOL WSAInitialized=FALSE;

static BOOL winsock_startup(void)
{
	int		status;             /* Status Code */

    if((status = WSAStartup(MAKEWORD(1,1), &WSAData))==0) {
		lprintf(LOG_DEBUG,"%s %s",WSAData.szDescription, WSAData.szSystemStatus);
		WSAInitialized=TRUE;
		return (TRUE);
	}

    lprintf(LOG_CRIT,"!WinSock startup ERROR %d", status);
	return (FALSE);
}

#else /* No WINSOCK */

#define winsock_startup()	(TRUE)
#define SOCKLIB_DESC NULL

#endif

static void update_clients(void)
{
	if(startup!=NULL && startup->clients!=NULL)
		startup->clients(startup->cbdata,active_clients.value+active_sendmail);
}

static void client_on(SOCKET sock, client_t* client, BOOL update)
{
	if(startup!=NULL && startup->client_on!=NULL)
		startup->client_on(startup->cbdata,TRUE,sock,client,update);
}

static void client_off(SOCKET sock)
{
	if(startup!=NULL && startup->client_on!=NULL)
		startup->client_on(startup->cbdata,FALSE,sock,NULL,FALSE);
}

static int32_t thread_up(BOOL setuid)
{
	int32_t	count =	protected_uint32_adjust(&thread_count,1);
	if(startup!=NULL && startup->thread_up!=NULL)
		startup->thread_up(startup->cbdata,TRUE,setuid);
	return count;
}

static int32_t thread_down(void)
{
	int32_t count = protected_uint32_adjust(&thread_count,-1);
	if(startup!=NULL && startup->thread_up!=NULL)
		startup->thread_up(startup->cbdata,FALSE,FALSE);
	return count;
}

SOCKET mail_open_socket(int type, const char* protocol)
{
	char	error[256];
	char	section[128];
	SOCKET	sock;

	sock=socket(AF_INET, type, IPPROTO_IP);
	if(sock!=INVALID_SOCKET && startup!=NULL && startup->socket_open!=NULL) 
		startup->socket_open(startup->cbdata,TRUE);
	if(sock!=INVALID_SOCKET) {
		SAFEPRINTF(section,"mail|%s",protocol);
		if(set_socket_options(&scfg, sock, section, error, sizeof(error)))
			lprintf(LOG_ERR,"%04d !ERROR %s",sock,error);

		stats.sockets++;
#if 0 /*def _DEBUG */
		lprintf(LOG_DEBUG,"%04d Socket opened (%d sockets in use)",sock,stats.sockets);
#endif
	}
	return(sock);
}

int mail_close_socket(SOCKET sock)
{
	int		result;

	if(sock==INVALID_SOCKET)
		return(-1);

	shutdown(sock,SHUT_RDWR);	/* required on Unix */
	result=closesocket(sock);
	if(startup!=NULL && startup->socket_open!=NULL)
		startup->socket_open(startup->cbdata,FALSE);
	stats.sockets--;
	if(result!=0) {
		if(ERROR_VALUE!=ENOTSOCK)
			lprintf(LOG_WARNING,"%04d !ERROR %d closing socket",sock, ERROR_VALUE);
	}
#if 0 /*def _DEBUG */
	else 
		lprintf(LOG_DEBUG,"%04d Socket closed (%d sockets in use)",sock,stats.sockets);
#endif

	return(result);
}

static void status(char* str)
{
	if(startup!=NULL && startup->status!=NULL)
	    startup->status(startup->cbdata,str);
}

int sockprintf(SOCKET sock, char *fmt, ...)
{
	int		len;
	int		maxlen;
	int		result;
	va_list argptr;
	char	sbuf[1024];
	fd_set	socket_set;
	struct timeval tv;

    va_start(argptr,fmt);
    len=vsnprintf(sbuf,maxlen=sizeof(sbuf)-2,fmt,argptr);
    va_end(argptr);

	if(len<0 || len > maxlen) /* format error or output truncated */
		len=maxlen;
	if(startup->options&MAIL_OPT_DEBUG_TX)
		lprintf(LOG_DEBUG,"%04d TX: %.*s", sock, len, sbuf);
	memcpy(sbuf+len,"\r\n",2);
	len+=2;

	if(sock==INVALID_SOCKET) {
		lprintf(LOG_WARNING,"!INVALID SOCKET in call to sockprintf");
		return(0);
	}

	/* Check socket for writability (using select) */
	tv.tv_sec=300;
	tv.tv_usec=0;

	FD_ZERO(&socket_set);
	FD_SET(sock,&socket_set);

	if((result=select(sock+1,NULL,&socket_set,NULL,&tv))<1) {
		if(result==0)
			lprintf(LOG_NOTICE,"%04d !TIMEOUT selecting socket for send"
				,sock);
		else
			lprintf(LOG_NOTICE,"%04d !ERROR %d selecting socket for send"
				,sock, ERROR_VALUE);
		return(0);
	}

	while((result=sendsocket(sock,sbuf,len))!=len) {
		if(result==SOCKET_ERROR) {
			if(ERROR_VALUE==EWOULDBLOCK) {
				YIELD();
				continue;
			}
			if(ERROR_VALUE==ECONNRESET) 
				lprintf(LOG_NOTICE,"%04d Connection reset by peer on send",sock);
			else if(ERROR_VALUE==ECONNABORTED) 
				lprintf(LOG_NOTICE,"%04d Connection aborted by peer on send",sock);
			else
				lprintf(LOG_NOTICE,"%04d !ERROR %d sending on socket",sock,ERROR_VALUE);
			return(0);
		}
		lprintf(LOG_WARNING,"%04d !ERROR: short send on socket: %d instead of %d",sock,result,len);
	}
	return(len);
}

static void sockerror(SOCKET socket, int rd, const char* action)
{
	if(rd==0) 
		lprintf(LOG_NOTICE,"%04d Socket closed by peer on %s"
			,socket, action);
	else if(rd==SOCKET_ERROR) {
		if(ERROR_VALUE==ECONNRESET) 
			lprintf(LOG_NOTICE,"%04d Connection reset by peer on %s"
				,socket, action);
		else if(ERROR_VALUE==ECONNABORTED) 
			lprintf(LOG_NOTICE,"%04d Connection aborted by peer on %s"
				,socket, action);
		else
			lprintf(LOG_NOTICE,"%04d !SOCKET ERROR %d on %s"
				,socket, ERROR_VALUE, action);
	} else
		lprintf(LOG_WARNING,"%04d !SOCKET ERROR: unexpected return value %d from %s"
			,socket, rd, action);
}


static int sockreadline(SOCKET socket, char* buf, int len)
{
	char	ch;
	int		i,rd=0;
	fd_set	socket_set;
	struct	timeval	tv;
	time_t	start;

	buf[0]=0;

	start=time(NULL);

	if(socket==INVALID_SOCKET) {
		lprintf(LOG_WARNING,"!INVALID SOCKET in call to sockreadline");
		return(-1);
	}
	
	while(rd<len-1) {

		if(server_socket==INVALID_SOCKET || terminate_server) {
			lprintf(LOG_WARNING,"%04d !ABORTING sockreadline",socket);
			return(-1);
		}

		tv.tv_sec=startup->max_inactivity;
		tv.tv_usec=0;

		FD_ZERO(&socket_set);
		FD_SET(socket,&socket_set);

		i=select(socket+1,&socket_set,NULL,NULL,&tv);

		if(i<1) {
			if(i==0) {
				if(startup->max_inactivity && (time(NULL)-start)>startup->max_inactivity) {
					lprintf(LOG_WARNING,"%04d !TIMEOUT in sockreadline (%u seconds):  INACTIVE SOCKET",socket,startup->max_inactivity);
					return(-1);
				}
				continue;
			}
			sockerror(socket,i,"select");
			return(-1);
		}
		i=recv(socket, &ch, 1, 0);
		if(i<1) {
			sockerror(socket,i,"receive");
			return(-1);
		}
		if(ch=='\n' /* && rd>=1 */ ) { /* Mar-9-2003: terminate on sole LF */
			break;
		}	
		buf[rd++]=ch;
	}
	if(rd>0 && buf[rd-1]=='\r')
		rd--;
	buf[rd]=0;
	
	return(rd);
}

static BOOL sockgetrsp(SOCKET socket, char* rsp, char *buf, int len)
{
	int rd;

	while(1) {
		rd = sockreadline(socket, buf, len);
		if(rd<1) {
			if(rd==0)
				lprintf(LOG_WARNING,"%04d !RECEIVED BLANK RESPONSE, Expected '%s'", socket, rsp);
			return(FALSE);
		}
		if(buf[3]=='-')	{ /* Multi-line response */
			if(startup->options&MAIL_OPT_DEBUG_RX_RSP) 
				lprintf(LOG_DEBUG,"%04d RX: %s",socket,buf);
			continue;
		}
		if(rsp!=NULL && strnicmp(buf,rsp,strlen(rsp))) {
			lprintf(LOG_WARNING,"%04d !INVALID RESPONSE: '%s' Expected: '%s'", socket, buf, rsp);
			return(FALSE);
		}
		break;
	}
	if(startup->options&MAIL_OPT_DEBUG_RX_RSP) 
		lprintf(LOG_DEBUG,"%04d RX: %s",socket,buf);
	return(TRUE);
}

/* RFC822: The maximum total length of a text line including the
   <CRLF> is 1000 characters (but not counting the leading
   dot duplicated for transparency). 

   POP3 (RFC1939) actually calls for a 512 byte line length limit!
*/
#define MAX_LINE_LEN	998		

static ulong sockmimetext(SOCKET socket, smbmsg_t* msg, char* msgtxt, ulong maxlines
						  ,str_list_t file_list, char* mime_boundary)
{
	char		toaddr[256]="";
	char		fromaddr[256]="";
	char		fromhost[256];
	char		msgid[256];
	char		date[64];
	uchar*		p;
	char*		np;
	char*		content_type=NULL;
	int			i;
	int			s;
	ulong		lines;
	int			len,tlen;

	/* HEADERS (in recommended order per RFC822 4.1) */

	if(msg->reverse_path!=NULL)
		if(!sockprintf(socket,"Return-Path: %s", msg->reverse_path))
			return(0);

	for(i=0;i<msg->total_hfields;i++)
		if(msg->hfield[i].type == SMTPRECEIVED && msg->hfield_dat[i]!=NULL) 
			if(!sockprintf(socket,"Received: %s", msg->hfield_dat[i]))
				return(0);

	if(!sockprintf(socket,"Date: %s",msgdate(msg->hdr.when_written,date)))
		return(0);

	if((p=smb_get_hfield(msg,RFC822FROM,NULL))!=NULL)
		s=sockprintf(socket,"From: %s",p);	/* use original RFC822 header field */
	else {
		if(msg->from_net.type==NET_QWK && msg->from_net.addr!=NULL)
			SAFEPRINTF2(fromaddr,"%s!%s"
				,(char*)msg->from_net.addr
				,usermailaddr(&scfg,fromhost,msg->from));
		else if(msg->from_net.type==NET_FIDO && msg->from_net.addr!=NULL)
			SAFECOPY(fromaddr,smb_faddrtoa((faddr_t *)msg->from_net.addr,NULL));
		else if(msg->from_net.type!=NET_NONE && msg->from_net.addr!=NULL)
			SAFECOPY(fromaddr,(char*)msg->from_net.addr);
		else 
			usermailaddr(&scfg,fromaddr,msg->from);
		if(fromaddr[0]=='<')
			s=sockprintf(socket,"From: \"%s\" %s",msg->from,fromaddr);
		else
			s=sockprintf(socket,"From: \"%s\" <%s>",msg->from,fromaddr);
	}
	if(!s)
		return(0);

	if(msg->from_org!=NULL || msg->from_net.type==NET_NONE)
		if(!sockprintf(socket,"Organization: %s"
			,msg->from_org==NULL ? scfg.sys_name : msg->from_org))
			return(0);

	if(!sockprintf(socket,"Subject: %s",msg->subj))
		return(0);

	if((p=smb_get_hfield(msg,RFC822TO,NULL))!=NULL)
		s=sockprintf(socket,"To: %s",p);	/* use original RFC822 header field */
	else {
		if(strchr(msg->to,'@')!=NULL || msg->to_net.addr==NULL)
			s=sockprintf(socket,"To: %s",msg->to);	/* Avoid double-@ */
		else if(msg->to_net.type==NET_INTERNET || msg->to_net.type==NET_QWK) {
			if(strchr((char*)msg->to_net.addr,'<')!=NULL)
				s=sockprintf(socket,"To: %s",(char*)msg->to_net.addr);
			else
				s=sockprintf(socket,"To: \"%s\" <%s>",msg->to,(char*)msg->to_net.addr);
		} else {
			usermailaddr(&scfg,toaddr,msg->to);
			s=sockprintf(socket,"To: \"%s\" <%s>",msg->to,toaddr);
		}
	}
	if(!s)
		return(0);
	if((p=smb_get_hfield(msg,SMB_CARBONCOPY,NULL))!=NULL)
		if(!sockprintf(socket,"CC: %s",p))
			return(0);
	np=NULL;
	if((p=smb_get_hfield(msg,RFC822REPLYTO,NULL))==NULL) {
		np=msg->replyto;
		if(msg->replyto_net.type==NET_INTERNET)
			p=msg->replyto_net.addr;
	}
	if(p!=NULL) {
		if(np!=NULL)
			s=sockprintf(socket,"Reply-To: \"%s\" <%s>",np,p);
		else 
			s=sockprintf(socket,"Reply-To: %s",p);
	}
	if(!s)
		return(0);
	if(!sockprintf(socket,"Message-ID: %s",get_msgid(&scfg,INVALID_SUB,msg,msgid,sizeof(msgid))))
		return(0);
	if(msg->reply_id!=NULL)
		if(!sockprintf(socket,"In-Reply-To: %s",msg->reply_id))
			return(0);

	/* non-standard, but documented (mostly) in draft-newman-msgheader-originfo-05 */
	sockprintf(socket,"Originator-Info: account=%s; login-id=%s; server=%s; client=%s; addr=%s; prot=%s; port=%s; time=%s"
		,msg->from_ext
		,smb_get_hfield(msg,SENDERUSERID,NULL)
		,smb_get_hfield(msg,SENDERSERVER,NULL)
		,smb_get_hfield(msg,SENDERHOSTNAME,NULL)
		,smb_get_hfield(msg,SENDERIPADDR,NULL)
		,smb_get_hfield(msg,SENDERPROTOCOL,NULL)
		,smb_get_hfield(msg,SENDERPORT,NULL)
		,smb_get_hfield(msg,SENDERTIME,NULL)
		);

    for(i=0;i<msg->total_hfields;i++) { 
		if(msg->hfield[i].type==RFC822HEADER) { 
			if(strnicmp((char*)msg->hfield_dat[i],"Content-Type:",13)==0)
				content_type=msg->hfield_dat[i];
			if(!sockprintf(socket,"%s",(char*)msg->hfield_dat[i]))
				return(0);
        }
    }
	/* Default MIME Content-Type for non-Internet messages */
	if(msg->from_net.type!=NET_INTERNET && content_type==NULL && startup->default_charset[0]) {
		/* No content-type specified, so assume IBM code-page 437 (full ex-ASCII) */
		sockprintf(socket,"Content-Type: text/plain; charset=%s", startup->default_charset);
		sockprintf(socket,"Content-Transfer-Encoding: 8bit");
	}

	if(strListCount(file_list)) {	/* File attachments */
        mimeheaders(socket,mime_boundary);
        sockprintf(socket,"");
        mimeblurb(socket,mime_boundary);
        sockprintf(socket,"");
        mimetextpartheader(socket,mime_boundary);
	}
	if(!sockprintf(socket,""))	/* Header Terminator */
		return(0);

	/* MESSAGE BODY */
	lines=0;
	np=msgtxt;
	while(*np && lines<maxlines) {
		len=0;
		while(len<MAX_LINE_LEN && *(np+len)!=0 && *(np+len)!='\n')
			len++;

		tlen=len;
		while(tlen && *(np+(tlen-1))<=' ') /* Takes care of '\r' or spaces */
			tlen--;

		if(!sockprintf(socket, "%s%.*s", *np=='.' ? ".":"", tlen, np))
			break;
		lines++;
		if(*(np+len)=='\r')
			len++;
		if(*(np+len)=='\n')
			len++;
		np+=len;
		/* release time-slices every x lines */
		if(startup->lines_per_yield
			&& !(lines%startup->lines_per_yield))	
			YIELD();
	}
	if(file_list!=NULL) {
		for(i=0;file_list[i];i++) { 
			sockprintf(socket,"");
			lprintf(LOG_INFO,"%04u MIME Encoding and sending %s",socket,file_list[i]);
			if(!mimeattach(socket,mime_boundary,file_list[i]))
				lprintf(LOG_ERR,"%04u !ERROR opening/encoding/sending %s",socket,file_list[i]);
			else {
				endmime(socket,mime_boundary);
				if(msg->hdr.auxattr&MSG_KILLFILE)
					if(remove(file_list[i])!=0)
						lprintf(LOG_WARNING,"%04u !ERROR %d removing %s",socket,errno,file_list[i]);
			}
		}
	}
    sockprintf(socket,".");	/* End of text */
	return(lines);
}

static ulong sockmsgtxt(SOCKET socket, smbmsg_t* msg, char* msgtxt, ulong maxlines)
{
	char		filepath[MAX_PATH+1];
	ulong		retval;
	char*		boundary=NULL;
	unsigned	i;
	str_list_t	file_list=NULL;
	str_list_t	split;

	if(msg->hdr.auxattr&MSG_FILEATTACH) {

		boundary = mimegetboundary();
		file_list = strListInit();

		/* Parse header fields */
		for(i=0;i<msg->total_hfields;i++)
	        if(msg->hfield[i].type==FILEATTACH) 
				strListPush(&file_list,(char*)msg->hfield_dat[i]);

		/* Parse subject (if necessary) */
		if(!strListCount(file_list)) {	/* filename(s) stored in subject */
			split=strListSplitCopy(NULL,msg->subj," ");
			if(split!=NULL) {
				for(i=0;split[i];i++) {
					if(msg->idx.to!=0)
						SAFEPRINTF3(filepath,"%sfile/%04u.in/%s"
							,scfg.data_dir,msg->idx.to,getfname(truncsp(split[i])));
					else
						SAFEPRINTF3(filepath,"%sfile/%04u.out/%s"
							,scfg.data_dir,msg->idx.from,getfname(truncsp(split[i])));
					strListPush(&file_list,filepath);
				}
				strListFree(&split);
			}
		}
    }

	retval = sockmimetext(socket,msg,msgtxt,maxlines,file_list,boundary);

	strListFree(&file_list);

	if(boundary!=NULL)
		free(boundary);

	return(retval);
}

static u_long resolve_ip(char *inaddr)
{
	char*		p;
	char*		addr;
	char		buf[128];
	HOSTENT*	host;

	SAFECOPY(buf,inaddr);
	addr=buf;
	if(*addr=='[' && *(p=lastchar(addr))==']') { /* Support [ip_address] notation */
		addr++;
		*p=0;
	}

	if(*addr==0)
		return((u_long)INADDR_NONE);

	for(p=addr;*p;p++)
		if(*p!='.' && !isdigit(*p))
			break;
	if(!(*p))
		return(inet_addr(addr));

	if((host=gethostbyname(inaddr))==NULL)
		return((u_long)INADDR_NONE);

	return(*((ulong*)host->h_addr_list[0]));
}

/****************************************************************************/
/* Consecutive failed login (possible password hack) attempt tracking		*/
/****************************************************************************/
/* Counter is global so it is tracked between multiple connections.			*/
/* Failed consecutive login attempts > 10 will generate a hacklog entry	and	*/
/* immediately disconnect (after the usual failed-login delay).				*/
/* A failed login from a different host resets the counter.					*/
/* A successful login from the same host resets the counter.				*/
/****************************************************************************/

static void badlogin(SOCKET sock, const char* prot, const char* resp, char* user, char* passwd, char* host, SOCKADDR_IN* addr)
{
	char	reason[128];
	ulong	count;

	if(addr!=NULL) {
		SAFEPRINTF(reason,"%s LOGIN", prot);
		count=loginFailure(startup->login_attempt_list, addr, prot, user, passwd);
		if(startup->login_attempt_hack_threshold && count>=startup->login_attempt_hack_threshold)
			hacklog(&scfg, reason, user, passwd, host, addr);
		if(startup->login_attempt_filter_threshold && count>=startup->login_attempt_filter_threshold)
			filter_ip(&scfg, (char*)prot, "- TOO MANY CONSECUTIVE FAILED LOGIN ATTEMPTS"
				,host, inet_ntoa(addr->sin_addr), user, /* fname: */NULL);
	}

	mswait(startup->login_attempt_delay);
	sockprintf(sock,(char*)resp);
}

static void pop3_thread(void* arg)
{
	char*		p;
	char		str[128];
	char		buf[512];
	char		host_name[128];
	char		host_ip[64];
	char		username[128];
	char		password[128];
	char		challenge[256];
	uchar		digest[MD5_DIGEST_SIZE];
	char*		response="";
	char*		msgtxt;
	int			i;
	int			rd;
	BOOL		activity=TRUE;
	BOOL		apop=FALSE;
	long		l;
	ulong		lines;
	ulong		lines_sent;
	ulong		login_attempts;
	uint32_t	msgs;
	long		msgnum;
	ulong		bytes;
	SOCKET		socket;
	HOSTENT*	host;
	smb_t		smb;
	smbmsg_t	msg;
	user_t		user;
	client_t	client;
	mail_t*		mail;
	pop3_t		pop3=*(pop3_t*)arg;

	SetThreadName("POP3");
	thread_up(TRUE /* setuid */);

	free(arg);

	socket=pop3.socket;

	if(startup->options&MAIL_OPT_DEBUG_POP3)
		lprintf(LOG_DEBUG,"%04d POP3 session thread started", socket);

#ifdef _WIN32
	if(startup->pop3_sound[0] && !(startup->options&MAIL_OPT_MUTE)) 
		PlaySound(startup->pop3_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif

	SAFECOPY(host_ip,inet_ntoa(pop3.client_addr.sin_addr));

	if(startup->options&MAIL_OPT_DEBUG_POP3)
		lprintf(LOG_INFO,"%04d POP3 connection accepted from: %s port %u"
			,socket, host_ip, ntohs(pop3.client_addr.sin_port));

	if(startup->options&MAIL_OPT_NO_HOST_LOOKUP)
		host=NULL;
	else
		host=gethostbyaddr((char *)&pop3.client_addr.sin_addr
			,sizeof(pop3.client_addr.sin_addr),AF_INET);

	if(host!=NULL && host->h_name!=NULL)
		SAFECOPY(host_name,host->h_name);
	else
		strcpy(host_name,"<no name>");

	if(!(startup->options&MAIL_OPT_NO_HOST_LOOKUP) && (startup->options&MAIL_OPT_DEBUG_POP3))
		lprintf(LOG_INFO,"%04d POP3 Hostname: %s", socket, host_name);

	if(trashcan(&scfg,host_ip,"ip")) {
		lprintf(LOG_NOTICE,"%04d !POP3 CLIENT IP ADDRESS BLOCKED: %s"
			,socket, host_ip);
		sockprintf(socket,"-ERR Access denied.");
		mail_close_socket(socket);
		thread_down();
		return;
	}

	if(trashcan(&scfg,host_name,"host")) {
		lprintf(LOG_NOTICE,"%04d !POP3 CLIENT HOSTNAME BLOCKED: %s"
			,socket, host_name);
		sockprintf(socket,"-ERR Access denied.");
		mail_close_socket(socket);
		thread_down();
		return;
	}

	protected_uint32_adjust(&active_clients, 1);
	update_clients();

	/* Initialize client display */
	client.size=sizeof(client);
	client.time=time32(NULL);
	SAFECOPY(client.addr,host_ip);
	SAFECOPY(client.host,host_name);
	client.port=ntohs(pop3.client_addr.sin_port);
	client.protocol="POP3";
	client.user="<unknown>";
	client_on(socket,&client,FALSE /* update */);

	SAFEPRINTF(str,"POP3: %s", host_ip);
	status(str);

	if(startup->login_attempt_throttle
		&& (login_attempts=loginAttempts(startup->login_attempt_list, &pop3.client_addr)) > 1) {
		lprintf(LOG_DEBUG,"%04d POP3 Throttling suspicious connection from: %s (%u login attempts)"
			,socket, inet_ntoa(pop3.client_addr.sin_addr), login_attempts);
		mswait(login_attempts*startup->login_attempt_throttle);
	}

	mail=NULL;

	do {
		memset(&smb,0,sizeof(smb));
		memset(&msg,0,sizeof(msg));
		memset(&user,0,sizeof(user));
		password[0]=0;

		srand((unsigned int)(time(NULL) ^ (time_t)GetCurrentThreadId()));	/* seed random number generator */
		rand();	/* throw-away first result */
		safe_snprintf(challenge,sizeof(challenge),"<%x%x%lx%lx@%.128s>"
			,rand(),socket,(ulong)time(NULL),clock(),startup->host_name);

		sockprintf(socket,"+OK Synchronet POP3 Server %s-%s Ready %s"
			,revision,PLATFORM_DESC,challenge);

		/* Requires USER command first */
		for(i=3;i;i--) {
			if(!sockgetrsp(socket,NULL,buf,sizeof(buf)))
				break;
			if(!strnicmp(buf,"USER ",5))
				break;
			if(!strnicmp(buf,"APOP ",5)) {
				apop=TRUE;
				break;
			}
			sockprintf(socket,"-ERR USER or APOP command expected");
		}
		if(!i || buf[0]==0)	/* no USER or APOP command received */
			break;

		p=buf+5;
		SKIP_WHITESPACE(p);
		if(apop) {
			if((response=strrchr(p,' '))!=NULL)
				*(response++)=0;
			else
				response=p;
		}
		SAFECOPY(username,p);
		if(!apop) {
			sockprintf(socket,"+OK");
			if(!sockgetrsp(socket,"PASS ",buf,sizeof(buf))) {
				sockprintf(socket,"-ERR PASS command expected");
				break;
			}
			p=buf+5;
			SKIP_WHITESPACE(p);
			SAFECOPY(password,p);
		}
		user.number=matchuser(&scfg,username,FALSE /*sysop_alias*/);
		if(!user.number) {
			if(scfg.sys_misc&SM_ECHO_PW)
				lprintf(LOG_NOTICE,"%04d !POP3 UNKNOWN USER: %s (password: %s)"
					,socket, username, password);
			else
				lprintf(LOG_NOTICE,"%04d !POP3 UNKNOWN USER: %s"
					,socket, username);
			badlogin(socket, client.protocol, pop_err, username, password, host_name, &pop3.client_addr);
			break;
		}
		if((i=getuserdat(&scfg, &user))!=0) {
			lprintf(LOG_ERR,"%04d !POP3 ERROR %d getting data on user (%s)"
				,socket, i, username);
			badlogin(socket, client.protocol, pop_err, NULL, NULL, NULL, NULL);
			break;
		}
		if(user.misc&(DELETED|INACTIVE)) {
			lprintf(LOG_NOTICE,"%04d !POP3 DELETED or INACTIVE user #%u (%s)"
				,socket, user.number, username);
			badlogin(socket, client.protocol, pop_err, NULL, NULL, NULL, NULL);
			break;
		}
		if(apop) {
			strlwr(user.pass);	/* this is case-sensitive, so convert to lowercase */
			strcat(challenge,user.pass);
			MD5_calc(digest,challenge,strlen(challenge));
			MD5_hex((BYTE*)str,digest);
			if(strcmp(str,response)) {
				lprintf(LOG_NOTICE,"%04d !POP3 %s FAILED APOP authentication"
					,socket,username);
#if 0
				lprintf(LOG_DEBUG,"%04d !POP3 digest data: %s",socket,challenge);
				lprintf(LOG_DEBUG,"%04d !POP3 calc digest: %s",socket,str);
				lprintf(LOG_DEBUG,"%04d !POP3 resp digest: %s",socket,response);
#endif
				badlogin(socket, client.protocol, pop_err, username, response, host_name, &pop3.client_addr);
				break;
			}
		} else if(stricmp(password,user.pass)) {
			if(scfg.sys_misc&SM_ECHO_PW)
				lprintf(LOG_NOTICE,"%04d !POP3 FAILED Password attempt for user %s: '%s' expected '%s'"
					,socket, username, password, user.pass);
			else
				lprintf(LOG_NOTICE,"%04d !POP3 FAILED Password attempt for user %s"
					,socket, username);
			badlogin(socket, client.protocol, pop_err, username, password, host_name, &pop3.client_addr);
			break;
		}

		if(user.pass[0])
			loginSuccess(startup->login_attempt_list, &pop3.client_addr);

		putuserrec(&scfg,user.number,U_COMP,LEN_COMP,host_name);
		putuserrec(&scfg,user.number,U_NOTE,LEN_NOTE,host_ip);

		/* Update client display */
		client.user=user.alias;
		client_on(socket,&client,TRUE /* update */);
		activity=FALSE;

		if(startup->options&MAIL_OPT_DEBUG_POP3)		
			lprintf(LOG_INFO,"%04d POP3 %s logged in %s", socket, user.alias, apop ? "via APOP":"");
		SAFEPRINTF(str,"POP3: %s",user.alias);
		status(str);

		SAFEPRINTF(smb.file,"%smail",scfg.data_dir);
		if(smb_islocked(&smb)) {
			lprintf(LOG_WARNING,"%04d !POP3 MAIL BASE LOCKED: %s",socket,smb.last_error);
			sockprintf(socket,"-ERR database locked, try again later");
			break;
		}
		smb.retry_time=scfg.smb_retry_time;
		smb.subnum=INVALID_SUB;
		if((i=smb_open(&smb))!=SMB_SUCCESS) {
			lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) opening %s",socket,i,smb.last_error,smb.file);
			sockprintf(socket,"-ERR %d opening %s",i,smb.file);
			break;
		}

		mail=loadmail(&smb,&msgs,user.number,MAIL_YOUR,0);

		for(l=bytes=0;l<msgs;l++) {
			msg.hdr.number=mail[l].number;
			if((i=smb_getmsgidx(&smb,&msg))!=SMB_SUCCESS) {
				lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message index"
					,socket, i, smb.last_error);
				break;
			}
			if((i=smb_lockmsghdr(&smb,&msg))!=SMB_SUCCESS) {
				lprintf(LOG_WARNING,"%04d !POP3 ERROR %d (%s) locking message header #%lu"
					,socket, i, smb.last_error, msg.hdr.number);
				break; 
			}
			i=smb_getmsghdr(&smb,&msg);
			smb_unlockmsghdr(&smb,&msg);
			if(i!=0) {
				lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message header #%lu"
					,socket, i, smb.last_error, msg.hdr.number);
				break;
			}
			bytes+=smb_getmsgtxtlen(&msg);
			smb_freemsgmem(&msg);
		}

		if(l<msgs) {
			sockprintf(socket,"-ERR message #%d: %d (%s)"
				,mail[l].number,i,smb.last_error);
			break;
		}

		sockprintf(socket,"+OK %lu messages (%lu bytes)",msgs,bytes);

		while(1) {	/* TRANSACTION STATE */
			rd = sockreadline(socket, buf, sizeof(buf));
			if(rd<0) 
				break;
			truncsp(buf);
			if(startup->options&MAIL_OPT_DEBUG_POP3)
				lprintf(LOG_DEBUG,"%04d POP3 RX: %s", socket, buf);
			if(!stricmp(buf, "NOOP")) {
				sockprintf(socket,"+OK");
				continue;
			}
			if(!stricmp(buf, "QUIT")) {
				sockprintf(socket,"+OK");
				break;
			}
			if(!stricmp(buf, "STAT")) {
				sockprintf(socket,"+OK %lu %lu",msgs,bytes);
				continue;
			}
			if(!stricmp(buf, "RSET")) {
				if((i=smb_locksmbhdr(&smb))!=SMB_SUCCESS) {
					lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) locking message base"
						,socket, i, smb.last_error);
					sockprintf(socket,"-ERR %d locking message base",i);
					continue;
				}
				for(l=0;l<msgs;l++) {
					msg.hdr.number=mail[l].number;
					if((i=smb_getmsgidx(&smb,&msg))!=SMB_SUCCESS) {
						lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message index"
							,socket, i, smb.last_error);
						break;
					}
					if((i=smb_lockmsghdr(&smb,&msg))!=SMB_SUCCESS) {
						lprintf(LOG_WARNING,"%04d !POP3 ERROR %d (%s) locking message header #%lu"
							,socket, i, smb.last_error, msg.hdr.number);
						break; 
					}
					if((i=smb_getmsghdr(&smb,&msg))!=SMB_SUCCESS) {
						smb_unlockmsghdr(&smb,&msg);
						lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message header #%lu"
							,socket, i, smb.last_error, msg.hdr.number);
						break;
					}
					msg.hdr.attr=mail[l].attr;
					if((i=smb_putmsg(&smb,&msg))!=SMB_SUCCESS)
						lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) updating message index"
							,socket, i, smb.last_error);
					smb_unlockmsghdr(&smb,&msg);
					smb_freemsgmem(&msg);
				}
				smb_unlocksmbhdr(&smb);

				if(l<msgs)
					sockprintf(socket,"-ERR %d messages reset (ERROR: %d)",l,i);
				else
					sockprintf(socket,"+OK %lu messages (%lu bytes)",msgs,bytes);
				continue;
			}
			if(!strnicmp(buf, "LIST",4) || !strnicmp(buf,"UIDL",4)) {
				p=buf+4;
				SKIP_WHITESPACE(p);
				if(isdigit(*p)) {
					msgnum=atol(p);
					if(msgnum<1 || msgnum>msgs) {
						lprintf(LOG_NOTICE,"%04d !POP3 INVALID message #%ld"
							,socket, msgnum);
						sockprintf(socket,"-ERR no such message");
						continue;
					}
					msg.hdr.number=mail[msgnum-1].number;
					if((i=smb_getmsgidx(&smb,&msg))!=SMB_SUCCESS) {
						lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message index"
							,socket, i, smb.last_error);
						sockprintf(socket,"-ERR %d getting message index",i);
						break;
					}
					if(msg.idx.attr&MSG_DELETE) {
						lprintf(LOG_NOTICE,"%04d !POP3 ATTEMPT to list DELETED message"
							,socket);
						sockprintf(socket,"-ERR message deleted");
						continue;
					}
					if((i=smb_lockmsghdr(&smb,&msg))!=SMB_SUCCESS) {
						lprintf(LOG_WARNING,"%04d !POP3 ERROR %d (%s) locking message header #%lu"
							,socket, i, smb.last_error, msg.hdr.number);
						sockprintf(socket,"-ERR %d locking message header",i);
						continue; 
					}
					i=smb_getmsghdr(&smb,&msg);
					smb_unlockmsghdr(&smb,&msg);
					if(i!=0) {
						smb_freemsgmem(&msg);
						lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message header #%lu"
							,socket, i, smb.last_error, msg.hdr.number);
						sockprintf(socket,"-ERR %d getting message header",i);
						continue;
					}
					if(!strnicmp(buf, "LIST",4)) {
						sockprintf(socket,"+OK %lu %lu",msgnum,smb_getmsgtxtlen(&msg));
					} else /* UIDL */
						sockprintf(socket,"+OK %lu %lu",msgnum,msg.hdr.number);

					smb_freemsgmem(&msg);
					continue;
				}
				/* List ALL messages */
				sockprintf(socket,"+OK %lu messages (%lu bytes)",msgs,bytes);
				for(l=0;l<msgs;l++) {
					msg.hdr.number=mail[l].number;
					if((i=smb_getmsgidx(&smb,&msg))!=SMB_SUCCESS) {
						lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message index"
							,socket, i, smb.last_error);
						break;
					}
					if(msg.idx.attr&MSG_DELETE) 
						continue;
					if((i=smb_lockmsghdr(&smb,&msg))!=SMB_SUCCESS) {
						lprintf(LOG_WARNING,"%04d !POP3 ERROR %d (%s) locking message header #%lu"
							,socket, i, smb.last_error, msg.hdr.number);
						break; 
					}
					i=smb_getmsghdr(&smb,&msg);
					smb_unlockmsghdr(&smb,&msg);
					if(i!=0) {
						smb_freemsgmem(&msg);
						lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message header #%lu"
							,socket, i, smb.last_error, msg.hdr.number);
						break;
					}
					if(!strnicmp(buf, "LIST",4)) {
						sockprintf(socket,"%lu %lu",l+1,smb_getmsgtxtlen(&msg));
					} else /* UIDL */
						sockprintf(socket,"%lu %lu",l+1,msg.hdr.number);

					smb_freemsgmem(&msg);
				}			
				sockprintf(socket,".");
				continue;
			}
			activity=TRUE;
			if(!strnicmp(buf, "RETR ",5) || !strnicmp(buf,"TOP ",4)) {
				SAFEPRINTF(str,"POP3: %s", user.alias);
				status(str);

				lines=-1;
				p=buf+4;
				SKIP_WHITESPACE(p);
				msgnum=atol(p);

				if(!strnicmp(buf,"TOP ",4)) {
					SKIP_DIGIT(p);
					SKIP_WHITESPACE(p);
					lines=atol(p);
				}
				if(msgnum<1 || msgnum>msgs) {
					lprintf(LOG_NOTICE,"%04d !POP3 %s ATTEMPTED to retrieve an INVALID message #%ld"
						,socket, user.alias, msgnum);
					sockprintf(socket,"-ERR no such message");
					continue;
				}
				msg.hdr.number=mail[msgnum-1].number;

				lprintf(LOG_INFO,"%04d POP3 %s retrieving message #%ld with command: %s"
					,socket, user.alias, msg.hdr.number, buf);

				if((i=smb_getmsgidx(&smb,&msg))!=SMB_SUCCESS) {
					lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message index"
						,socket, i, smb.last_error);
					sockprintf(socket,"-ERR %d getting message index",i);
					continue;
				}
				if(msg.idx.attr&MSG_DELETE) {
					lprintf(LOG_NOTICE,"%04d !POP3 ATTEMPT to retrieve DELETED message"
						,socket);
					sockprintf(socket,"-ERR message deleted");
					continue;
				}
				if((i=smb_lockmsghdr(&smb,&msg))!=SMB_SUCCESS) {
					lprintf(LOG_WARNING,"%04d !POP3 ERROR %d (%s) locking message header #%lu"
						,socket, i, smb.last_error, msg.hdr.number);
					sockprintf(socket,"-ERR %d locking message header",i);
					continue; 
				}
				i=smb_getmsghdr(&smb,&msg);
				smb_unlockmsghdr(&smb,&msg);
				if(i!=0) {
					lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message header #%lu"
						,socket, i, smb.last_error, msg.hdr.number);
					sockprintf(socket,"-ERR %d getting message header",i);
					continue;
				}

				if((msgtxt=smb_getmsgtxt(&smb,&msg,GETMSGTXT_ALL))==NULL) {
					smb_freemsgmem(&msg);
					lprintf(LOG_ERR,"%04d !POP3 ERROR (%s) retrieving message %lu text"
						,socket, smb.last_error, msg.hdr.number);
					sockprintf(socket,"-ERR retrieving message text");
					continue;
				}

				remove_ctrl_a(msgtxt, msgtxt);

				if(lines > 0					/* Works around BlackBerry mail server */
					&& lines >= strlen(msgtxt))	/* which requests the number of bytes (instead of lines) using TOP */
					lines=-1;					

				sockprintf(socket,"+OK message follows");
				lprintf(LOG_DEBUG,"%04d POP3 sending message text (%u bytes)"
					,socket,strlen(msgtxt));
				lines_sent=sockmsgtxt(socket,&msg,msgtxt,lines);
				/* if(startup->options&MAIL_OPT_DEBUG_POP3) */
				if(lines!=-1 && lines_sent<lines)	/* could send *more* lines */
					lprintf(LOG_ERR,"%04d !POP3 ERROR sending message text (sent %ld of %ld lines)"
						,socket,lines_sent,lines);
				else {
					lprintf(LOG_DEBUG,"%04d POP3 message transfer complete (%lu lines)"
						,socket,lines_sent);

					if((i=smb_locksmbhdr(&smb))!=SMB_SUCCESS) {
						lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) locking message base"
							,socket, i, smb.last_error);
					} else {
						if((i=smb_getmsgidx(&smb,&msg))!=SMB_SUCCESS) {
							lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message index"
								,socket, i, smb.last_error);
						} else {
							msg.hdr.attr|=MSG_READ;
							msg.hdr.netattr|=MSG_SENT;

							if((i=smb_lockmsghdr(&smb,&msg))!=SMB_SUCCESS) 
								lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) locking message header #%lu"
									,socket, i, smb.last_error, msg.hdr.number);
							if((i=smb_putmsg(&smb,&msg))!=SMB_SUCCESS)
								lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) marking message #%lu as read"
									,socket, i, smb.last_error, msg.hdr.number);
							smb_unlockmsghdr(&smb,&msg);
						}
						smb_unlocksmbhdr(&smb);
					}
				}
				smb_freemsgmem(&msg);
				smb_freemsgtxt(msgtxt);
				continue;
			}
			if(!strnicmp(buf, "DELE ",5)) {
				p=buf+5;
				SKIP_WHITESPACE(p);
				msgnum=atol(p);

				if(msgnum<1 || msgnum>msgs) {
					lprintf(LOG_NOTICE,"%04d !POP3 %s ATTEMPTED to delete an INVALID message #%ld"
						,socket, user.alias, msgnum);
					sockprintf(socket,"-ERR no such message");
					continue;
				}
				msg.hdr.number=mail[msgnum-1].number;

				lprintf(LOG_INFO,"%04d POP3 %s deleting message #%ld"
					,socket, user.alias, msg.hdr.number);

				if((i=smb_locksmbhdr(&smb))!=SMB_SUCCESS) {
					lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) locking message base"
						,socket, i, smb.last_error);
					sockprintf(socket,"-ERR %d locking message base",i);
					continue;
				}
				if((i=smb_getmsgidx(&smb,&msg))!=SMB_SUCCESS) {
					smb_unlocksmbhdr(&smb);
					lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message index"
						,socket, i, smb.last_error);
					sockprintf(socket,"-ERR %d getting message index",i);
					continue;
				}
				if((i=smb_lockmsghdr(&smb,&msg))!=SMB_SUCCESS) {
					smb_unlocksmbhdr(&smb);
					lprintf(LOG_WARNING,"%04d !POP3 ERROR %d (%s) locking message header #%lu"
						,socket, i, smb.last_error, msg.hdr.number);
					sockprintf(socket,"-ERR %d locking message header",i);
					continue; 
				}
				if((i=smb_getmsghdr(&smb,&msg))!=SMB_SUCCESS) {
					smb_unlockmsghdr(&smb,&msg);
					smb_unlocksmbhdr(&smb);
					lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) getting message header #%lu"
						,socket, i, smb.last_error, msg.hdr.number);
					sockprintf(socket,"-ERR %d getting message header",i);
					continue;
				}
				msg.hdr.attr|=MSG_DELETE;

				if((i=smb_putmsg(&smb,&msg))==SMB_SUCCESS && msg.hdr.auxattr&MSG_FILEATTACH)
					delfattach(&scfg,&msg);
				smb_unlockmsghdr(&smb,&msg);
				smb_unlocksmbhdr(&smb);
				smb_freemsgmem(&msg);
				if(i!=SMB_SUCCESS) {
					lprintf(LOG_ERR,"%04d !POP3 ERROR %d (%s) marking message as read"
						, socket, i, smb.last_error);
					sockprintf(socket,"-ERR %d marking message for deletion",i);
					continue;
				}
				sockprintf(socket,"+OK");
				if(startup->options&MAIL_OPT_DEBUG_POP3)
					lprintf(LOG_INFO,"%04d POP3 message deleted", socket);
				continue;
			}
			lprintf(LOG_NOTICE,"%04d !POP3 UNSUPPORTED COMMAND from %s: '%s'"
				,socket, user.alias, buf);
			sockprintf(socket,"-ERR UNSUPPORTED COMMAND: %s",buf);
		}
		if(user.number) {
			if(!logoutuserdat(&scfg,&user,time(NULL),client.time))
				lprintf(LOG_ERR,"%04d !ERROR in logoutuserdat", socket);
		}

	} while(0);

	if(activity) {
		if(user.number)
			lprintf(LOG_INFO,"%04d POP3 %s logged out from port %u on %s [%s]"
				,socket, user.alias, ntohs(pop3.client_addr.sin_port), host_name, host_ip);
		else
			lprintf(LOG_INFO,"%04d POP3 client disconnected from port %u on %s [%s]"
				,socket, ntohs(pop3.client_addr.sin_port), host_name, host_ip);
	}

	status(STATUS_WFC);

	/* Free up resources here */
	if(mail!=NULL)
		freemail(mail);

	smb_freemsgmem(&msg);
	smb_close(&smb);

	protected_uint32_adjust(&active_clients, -1);
	update_clients();
	client_off(socket);

	{ 
		int32_t remain = thread_down();
		if(startup->options&MAIL_OPT_DEBUG_POP3)
			lprintf(LOG_DEBUG,"%04d POP3 session thread terminated (%u threads remain, %lu clients served)"
				,socket, remain, ++stats.pop3_served);
	}

	/* Must be last */
	mail_close_socket(socket);
}

static ulong rblchk(SOCKET sock, DWORD mail_addr_n, const char* rbl_addr)
{
	char		name[256];
	DWORD		mail_addr;
	HOSTENT*	host;
	struct in_addr dnsbl_result;

	mail_addr=ntohl(mail_addr_n);
	safe_snprintf(name,sizeof(name),"%ld.%ld.%ld.%ld.%.128s"
		,mail_addr&0xff
		,(mail_addr>>8)&0xff
		,(mail_addr>>16)&0xff
		,(mail_addr>>24)&0xff
		,rbl_addr
		);

	lprintf(LOG_DEBUG,"%04d SMTP DNSBL Query: %s",sock,name);

	if((host=gethostbyname(name))==NULL)
		return(0);

	dnsbl_result.s_addr = *((ulong*)host->h_addr_list[0]);
	lprintf(LOG_INFO,"%04d SMTP DNSBL Query: %s resolved to: %s"
		,sock,name,inet_ntoa(dnsbl_result));

	return(dnsbl_result.s_addr);
}

static ulong dns_blacklisted(SOCKET sock, IN_ADDR addr, char* host_name, char* list, char* dnsbl_ip)
{
	char	fname[MAX_PATH+1];
	char	str[256];
	char*	p;
	char*	tp;
	FILE*	fp;
	ulong	found=0;

	SAFEPRINTF(fname,"%sdnsbl_exempt.cfg",scfg.ctrl_dir);
	if(findstr(inet_ntoa(addr),fname))
		return(FALSE);
	if(findstr(host_name,fname))
		return(FALSE);

	SAFEPRINTF(fname,"%sdns_blacklist.cfg", scfg.ctrl_dir);
	if((fp=fopen(fname,"r"))==NULL)
		return(FALSE);

	while(!feof(fp) && !found) {
		if(fgets(str,sizeof(str),fp)==NULL)
			break;
		truncsp(str);

		p=str;
		SKIP_WHITESPACE(p);
		if(*p==';' || *p==0) /* comment or blank line */
			continue;

		sprintf(list,"%.100s",p);

		/* terminate */
		tp = p;
		FIND_WHITESPACE(tp);
		*tp=0;	

		found = rblchk(sock, addr.s_addr, p);
	}
	fclose(fp);
	if(found)
		strcpy(dnsbl_ip, inet_ntoa(addr));

	return(found);
}


static BOOL chk_email_addr(SOCKET socket, char* p, char* host_name, char* host_ip
						   ,char* to, char* from, char* source)
{
	char	addr[64];
	char	tmp[128];

	SKIP_WHITESPACE(p);
	if(*p=='<') p++;		/* Skip '<' */
	SAFECOPY(addr,p);
	truncstr(addr,">( ");

	if(!trashcan(&scfg,addr,"email"))
		return(TRUE);

	lprintf(LOG_NOTICE,"%04d !SMTP BLOCKED %s e-mail address: %s"
		,socket, source, addr);
	SAFEPRINTF2(tmp,"Blocked %s e-mail address: %s", source, addr);
	spamlog(&scfg, "SMTP", "REFUSED", tmp, host_name, host_ip, to, from);

	return(FALSE);
}

static BOOL email_addr_is_exempt(const char* addr)
{
	char fname[MAX_PATH+1];
	char netmail[128];
	char* p;

	if(*addr==0 || strcmp(addr,"<>")==0)
		return FALSE;
	SAFEPRINTF(fname,"%sdnsbl_exempt.cfg",scfg.ctrl_dir);
	if(findstr((char*)addr,fname))
		return TRUE;
	SAFECOPY(netmail, addr);
	if(*(p=netmail)=='<')
		p++;
	truncstr(p,">");
	return userdatdupe(&scfg, 0, U_NETMAIL, LEN_NETMAIL, p, /* del */FALSE, /* next */FALSE);
}

static void exempt_email_addr(const char* comment
							  ,const char* fromname, const char* fromext, const char* fromaddr
							  ,const char* toaddr)
{
	char	fname[MAX_PATH+1];
	char	to[128];
	char	tmp[128];
	FILE*	fp;

	SAFEPRINTF(to,"<%s>",toaddr);
	if(!email_addr_is_exempt(to)) {
		SAFEPRINTF(fname,"%sdnsbl_exempt.cfg",scfg.ctrl_dir);
		if((fp=fopen(fname,"a"))==NULL)
			lprintf(LOG_ERR,"0000 !Error opening file: %s", fname);
		else {
			lprintf(LOG_INFO,"0000 %s: %s", comment, to);
			fprintf(fp,"\n;%s from \"%s\""
				,comment, fromname);
			if(fromext!=NULL)
				fprintf(fp,"%s",fromext);
			fprintf(fp," %s on %s\n%s\n"
				,fromaddr, timestr(&scfg,time32(NULL),tmp), to);
			fclose(fp);
		}
	}
}

static void signal_smtp_sem(void)
{
	int file;

	if(scfg.smtpmail_sem[0]==0) 
		return; /* do nothing */

	if((file=open(scfg.smtpmail_sem,O_WRONLY|O_CREAT|O_TRUNC,DEFFILEMODE))!=-1)
		close(file);
}

/*****************************************************************************/
/* Returns command line generated from instr with %c replacments             */
/*****************************************************************************/
static char* mailcmdstr(char* instr, char* msgpath, char* newpath, char* logpath
						,char* lstpath, char* errpath
						,char* host, char* ip, uint usernum
						,char* rcpt_addr
						,char* sender, char* sender_addr, char* reverse_path, char* cmd)
{
	char	str[1024];
    int		i,j,len;

    len=strlen(instr);
    for(i=j=0;i<len;i++) {
        if(instr[i]=='%') {
            i++;
            cmd[j]=0;
            switch(toupper(instr[i])) {
				case 'D':
					strcat(cmd,logpath);
					break;
				case 'E':
					strcat(cmd,errpath);
					break;
				case 'H':
					strcat(cmd,host);
					break;
				case 'I':
					strcat(cmd,ip);
					break;
                case 'G':   /* Temp directory */
                    strcat(cmd,scfg.temp_dir);
                    break;
                case 'J':
                    strcat(cmd,scfg.data_dir);
                    break;
                case 'K':
                    strcat(cmd,scfg.ctrl_dir);
                    break;
				case 'L':
					strcat(cmd,lstpath);
					break;
				case 'F':
				case 'M':
					strcat(cmd,msgpath);
					break;
				case 'N':
					strcat(cmd,newpath);
					break;
                case 'O':   /* SysOp */
                    strcat(cmd,scfg.sys_op);
                    break;
                case 'Q':   /* QWK ID */
                    strcat(cmd,scfg.sys_id);
                    break;
				case 'R':	/* reverse path */
					strcat(cmd,reverse_path);
					break;
				case 'S':	/* sender name */
					strcat(cmd,sender);
					break;
				case 'T':	/* recipient */
					strcat(cmd,rcpt_addr);
					break;
				case 'A':	/* sender address */
					strcat(cmd,sender_addr);
					break;
                case 'V':   /* Synchronet Version */
                    SAFEPRINTF2(str,"%s%c",VERSION,REVISION);
					strcat(cmd,str);
                    break;
                case 'Z':
                    strcat(cmd,scfg.text_dir);
                    break;
                case '!':   /* EXEC Directory */
                    strcat(cmd,scfg.exec_dir);
                    break;
                case '@':   /* EXEC Directory for DOS/OS2/Win32, blank for Unix */
#ifndef __unix__
                    strcat(cmd,scfg.exec_dir);
#endif
                    break;
                case '%':   /* %% for percent sign */
                    strcat(cmd,"%");
                    break;
				case '?':	/* Platform */
#ifdef __OS2__
					SAFECOPY(str,"OS2");
#else
					SAFECOPY(str,PLATFORM_DESC);
#endif
					strlwr(str);
					strcat(cmd,str);
					break;
				case 'U':	/* User number */
					SAFEPRINTF(str,"%u",usernum);
					strcat(cmd,str);
					break;
                default:    /* unknown specification */
                    break; 
			}
            j=strlen(cmd); 
		}
        else
            cmd[j++]=instr[i]; 
	}
    cmd[j]=0;

    return(cmd);
}
#ifdef JAVASCRIPT

typedef struct {
	SOCKET		sock;
	const char*	log_prefix;
	const char*	proc_name;
} private_t;

static void
js_ErrorReporter(JSContext *cx, const char *message, JSErrorReport *report)
{
	char	line[64];
	char	file[MAX_PATH+1];
	char*	warning;
	private_t*	p;
	jsrefcount	rc;
	int		log_level;

	if((p=(private_t*)JS_GetContextPrivate(cx))==NULL)
		return;

	if(report==NULL) {
		lprintf(LOG_ERR,"%04d %s %s !JavaScript: %s"
			, p->sock, p->log_prefix, p->proc_name, message);
		return;
    }

	if(report->filename)
		SAFEPRINTF(file," %s",report->filename);
	else
		file[0]=0;

	if(report->lineno)
		SAFEPRINTF(line," line %u",report->lineno);
	else
		line[0]=0;

	if(JSREPORT_IS_WARNING(report->flags)) {
		if(JSREPORT_IS_STRICT(report->flags))
			warning="strict warning";
		else
			warning="warning";
		log_level=LOG_WARNING;
	} else {
		log_level=LOG_ERR;
		warning="";
	}

	rc=JS_SUSPENDREQUEST(cx);
	lprintf(log_level,"%04d %s %s !JavaScript %s%s%s: %s"
		,p->sock, p->log_prefix, p->proc_name
		,warning ,file, line, message);
	JS_RESUMEREQUEST(cx, rc);
}

static JSBool
js_log(JSContext *cx, uintN argc, jsval *arglist)
{
	jsval *argv=JS_ARGV(cx, arglist);
    uintN		i=0;
	int32		level=LOG_INFO;
	private_t*	p;
	jsrefcount	rc;
	char		*lstr=NULL;
	size_t		lstr_sz=0;

	JS_SET_RVAL(cx, arglist, JSVAL_VOID);

	if((p=(private_t*)JS_GetContextPrivate(cx))==NULL)
		return(JS_FALSE);

	if(JSVAL_IS_NUMBER(argv[i])) {
		if(!JS_ValueToInt32(cx,argv[i++],&level))
			return JS_FALSE;
	}

	for(; i<argc; i++) {
		JSVALUE_TO_RASTRING(cx, argv[i], lstr, &lstr_sz, NULL);
		HANDLE_PENDING(cx);
		if(lstr==NULL)
			return(JS_TRUE);
		rc=JS_SUSPENDREQUEST(cx);
		lprintf(level,"%04d %s %s %s"
			,p->sock,p->log_prefix,p->proc_name,lstr);
		JS_RESUMEREQUEST(cx, rc);
	}

	if(lstr)
		free(lstr);

    return(JS_TRUE);
}

static JSFunctionSpec js_global_functions[] = {
	{"write",			js_log,				0},
	{"writeln",			js_log,				0},
	{"print",			js_log,				0},
	{"log",				js_log,				0},
    {0}
};

static BOOL
js_mailproc(SOCKET sock, client_t* client, user_t* user, struct mailproc* mailproc
			,char* cmdline
			,char* msgtxt_fname, char* newtxt_fname, char* logtxt_fname
			,char* rcpt_addr
			,char* rcptlst_fname, char* proc_err_fname
			,char* sender, char* sender_addr, char* reverse_path, char* hello_name
			,int32* result
			,JSRuntime**	js_runtime
			,JSContext**	js_cx
			,JSObject**		js_glob
			,const char*	log_prefix
)
{
	char*		p;
	char		fname[MAX_PATH+1];
	char		path[MAX_PATH+1];
	char		arg[MAX_PATH+1];
	BOOL		success=FALSE;
	JSObject*	js_scope=NULL;
	JSObject*	argv;
	jsuint		argc;
	JSObject*	js_script;
	js_callback_t	js_callback;
	jsval		val;
	jsval		rval=JSVAL_VOID;
	private_t	priv;

	ZERO_VAR(js_callback);

	SAFECOPY(fname,cmdline);
	truncstr(fname," \t");
	if(getfext(fname)==NULL) /* No extension specified, assume '.js' */
		strcat(fname,".js");

	SAFECOPY(path,fname);
	if(getfname(path)==path) { /* No path specified, assume mods or exec dir */
		SAFEPRINTF2(path,"%s%s",scfg.mods_dir,fname);
		if(scfg.mods_dir[0]==0 || !fexist(path))
			SAFEPRINTF2(path,"%s%s",scfg.exec_dir,fname);
	}

	*result = 0;
	do {
		if(*js_runtime==NULL) {
			lprintf(LOG_DEBUG,"%04d %s JavaScript: Creating runtime: %lu bytes\n"
				,sock, log_prefix, startup->js.max_bytes);

			if((*js_runtime = jsrt_GetNew(startup->js.max_bytes, 1000, __FILE__, __LINE__))==NULL)
				return FALSE;
		}

		if(*js_cx==NULL) {
			lprintf(LOG_DEBUG,"%04d %s JavaScript: Initializing context (stack: %lu bytes)\n"
				,sock, log_prefix, startup->js.cx_stack);

			if((*js_cx = JS_NewContext(*js_runtime, startup->js.cx_stack))==NULL)
				return FALSE;
		}
		JS_BEGINREQUEST(*js_cx);

		JS_SetErrorReporter(*js_cx, js_ErrorReporter);

		priv.sock=sock;
		priv.log_prefix=log_prefix;
		priv.proc_name=mailproc->name;
		JS_SetContextPrivate(*js_cx, &priv);

		if(*js_glob==NULL) {
			/* Global Objects (including system, js, client, Socket, MsgBase, File, User, etc. */
			if(!js_CreateCommonObjects(*js_cx, &scfg, &scfg, NULL
						,uptime, startup->host_name, SOCKLIB_DESC	/* system */
						,&js_callback									/* js */
						,&startup->js
						,client, sock								/* client */
						,&js_server_props							/* server */
						,js_glob
				))
				break;

			if(!JS_DefineFunctions(*js_cx, *js_glob, js_global_functions))
				break;

			/* Area and "user" Objects */
			if(!js_CreateUserObjects(*js_cx, *js_glob, &scfg, user, client, NULL, NULL)) 
				break;

			/* Mailproc "API" filenames */
			JS_DefineProperty(*js_cx, *js_glob, "message_text_filename"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,msgtxt_fname))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "new_message_text_filename"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,newtxt_fname))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "log_text_filename"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,logtxt_fname))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "recipient_address"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,rcpt_addr))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "recipient_list_filename"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,rcptlst_fname))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "processing_error_filename"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,proc_err_fname))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "sender_name"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,sender))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "sender_address"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,sender_addr))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "reverse_path"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,reverse_path))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

			JS_DefineProperty(*js_cx, *js_glob, "hello_name"
				,STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,hello_name))
				,NULL,NULL,JSPROP_ENUMERATE|JSPROP_READONLY);

		}

		if((js_scope=JS_NewObject(*js_cx, NULL, NULL, *js_glob))==NULL)
			break;

		/* Convert command-line to argv/argc */
		argv=JS_NewArrayObject(*js_cx, 0, NULL);
		JS_DefineProperty(*js_cx, js_scope, "argv", OBJECT_TO_JSVAL(argv)
			,NULL,NULL,JSPROP_READONLY|JSPROP_ENUMERATE);

		p=cmdline;
		FIND_WHITESPACE(p); 
		SKIP_WHITESPACE(p);
		for(argc=0;*p;argc++) {
			SAFECOPY(arg,p);
			truncstr(arg," \t");
			val=STRING_TO_JSVAL(JS_NewStringCopyZ(*js_cx,arg));
			if(!JS_SetElement(*js_cx, argv, argc, &val))
				break;
			FIND_WHITESPACE(p);
			SKIP_WHITESPACE(p);
		}
		JS_DefineProperty(*js_cx, js_scope, "argc", INT_TO_JSVAL(argc)
			,NULL,NULL,JSPROP_READONLY|JSPROP_ENUMERATE);

		if(mailproc->eval!=NULL && *mailproc->eval!=0) {
			lprintf(LOG_DEBUG,"%04d %s Evaluating: %s"
				,sock, log_prefix, mailproc->eval);
			js_script=JS_CompileScript(*js_cx, js_scope, mailproc->eval, strlen(mailproc->eval), NULL, 1);
		} else {
			lprintf(LOG_DEBUG,"%04d %s Executing: %s"
				,sock, log_prefix, cmdline);
			if((js_script=JS_CompileFile(*js_cx, js_scope, path)) != NULL)
				js_PrepareToExecute(*js_cx, js_scope, path, /* startup_dir: */NULL);
		}
		if(js_script==NULL)
			break;

		/* ToDo: Set operational callback */
		success=JS_ExecuteScript(*js_cx, js_scope, js_script, &rval);

		JS_GetProperty(*js_cx, *js_glob, "exit_code", &rval);

		if(rval!=JSVAL_VOID && JSVAL_IS_NUMBER(rval))
			JS_ValueToInt32(*js_cx,rval,result);

		js_EvalOnExit(*js_cx, js_scope, &js_callback);

		JS_ReportPendingException(*js_cx);

		JS_ClearScope(*js_cx, js_scope);

		JS_GC(*js_cx);

	} while(0);

	JS_ENDREQUEST(*js_cx);

	return(success);
}

void js_cleanup(JSRuntime* js_runtime, JSContext* js_cx, JSObject** js_glob)
{
	if(js_cx!=NULL) {
		JS_BEGINREQUEST(js_cx);
		JS_RemoveObjectRoot(js_cx, js_glob);
		JS_ENDREQUEST(js_cx);
		JS_DestroyContext(js_cx);
	}
	if(js_runtime!=NULL)
		jsrt_Release(js_runtime);
}
#endif

static char* get_header_field(char* buf, char* name, size_t maxlen)
{
	char*	p;
	size_t	len;

	if(buf[0]<=' ')	/* folded header */
		return NULL;

	if((p=strchr(buf,':'))==NULL)
		return NULL;

	len = p-buf;
	if(len >= maxlen)
		len = maxlen-1;
	sprintf(name,"%.*s",len,buf);
	truncsp(name);

	p++;	/* skip colon */
	SKIP_WHITESPACE(p);
	return p;
}

static int parse_header_field(char* buf, smbmsg_t* msg, ushort* type)
{
	char*	p;
	char*	tp;
	char	field[128];
	int		len;
	ushort	nettype;

	if(buf[0]<=' ' && *type!=UNKNOWN) {	/* folded header, append to previous */
		p=buf;
		truncsp(p);
		if(*type==RFC822HEADER || *type==SMTPRECEIVED)
			smb_hfield_append_str(msg,*type,"\r\n");
		else { /* Unfold other common header field types (e.g. Subject, From, To) */
			smb_hfield_append_str(msg,*type," ");
			SKIP_WHITESPACE(p);
		}
		return smb_hfield_append_str(msg, *type, p);
	}

	if((p=strchr(buf,':'))==NULL)
		return smb_hfield_str(msg, *type=RFC822HEADER, buf);

	len=(ulong)p-(ulong)buf;
	if(len>sizeof(field)-1)
		len=sizeof(field)-1;
	sprintf(field,"%.*s",len,buf);
	truncsp(field);

	p++;	/* skip colon */
	SKIP_WHITESPACE(p);
	truncsp(p);

	if(!stricmp(field, "TO"))
		return smb_hfield_str(msg, *type=RFC822TO, p);

	if(!stricmp(field, "REPLY-TO")) {
		smb_hfield_str(msg, *type=RFC822REPLYTO, p);
		if((tp=strrchr(p,'<'))!=NULL)  {
			tp++;
			truncstr(tp,">");
			p=tp;
		}
		nettype=NET_INTERNET;
		smb_hfield(msg, REPLYTONETTYPE, sizeof(nettype), &nettype);
		return smb_hfield_str(msg, *type=REPLYTONETADDR, p);
	}
	if(!stricmp(field, "FROM"))
		return smb_hfield_str(msg, *type=RFC822FROM, p);

	if(!stricmp(field, "ORGANIZATION"))
		return smb_hfield_str(msg, *type=SENDERORG, p);

	if(!stricmp(field, "DATE")) {
		msg->hdr.when_written=rfc822date(p);
		*type=UNKNOWN;
		return SMB_SUCCESS;
	}
	if(!stricmp(field, "MESSAGE-ID"))
		return smb_hfield_str(msg, *type=RFC822MSGID, p);

	if(!stricmp(field, "IN-REPLY-TO"))
		return smb_hfield_str(msg, *type=RFC822REPLYID, p);

	if(!stricmp(field, "CC"))
		return smb_hfield_str(msg, *type=SMB_CARBONCOPY, p);

	if(!stricmp(field, "RECEIVED"))
		return smb_hfield_str(msg, *type=SMTPRECEIVED, p);

	if(!stricmp(field, "RETURN-PATH")) {
		*type=UNKNOWN;
		return SMB_SUCCESS;	/* Ignore existing "Return-Path" header fields */
	}

	/* Fall-through */
	return smb_hfield_str(msg, *type=RFC822HEADER, buf);
}

static int chk_received_hdr(SOCKET socket,const char *buf,IN_ADDR *dnsbl_result, char *dnsbl, char *dnsbl_ip)
{
	char		host_name[128];
	IN_ADDR		check_addr;
	char		*fromstr;
	char		ip[16];
	char		*p;
	char		*p2;
	char		*last;

	fromstr=(char *)malloc(strlen(buf)+1);
	if(fromstr==NULL)
		return(0);
	strcpy(fromstr,buf);
	strlwr(fromstr);
	do {
		p=strstr(fromstr,"from ");
		if(p==NULL)
			break;
		p+=4;
		SKIP_WHITESPACE(p);
		if(*p==0)
			break;
		p2=host_name;
		for(;*p && !isspace((unsigned char)*p) && p2<host_name+126;p++)  {
			*p2++=*p;
		}
		*p2=0;
		p=strtok_r(fromstr,"[",&last);
		if(p==NULL)
			break;
		p=strtok_r(NULL,"]",&last);
		if(p==NULL)
			break;
		strncpy(ip,p,16);
		ip[15]=0;
		check_addr.s_addr = inet_addr(ip);
		lprintf(LOG_DEBUG,"%04d SMTP DNSBL checking received header address %s [%s]",socket,host_name,ip);
		if((dnsbl_result->s_addr=dns_blacklisted(socket,check_addr,host_name,dnsbl,dnsbl_ip))!=0)
				lprintf(LOG_NOTICE,"%04d SMTP BLACKLISTED SERVER on %s: %s [%s] = %s"
					,socket, dnsbl, host_name, ip, inet_ntoa(*dnsbl_result));
	} while(0);
	free(fromstr);
	return(dnsbl_result->s_addr);
}

static void parse_mail_address(char* p
							   ,char* name, size_t name_len
							   ,char* addr, size_t addr_len)
{
	char*	tp;
	char	tmp[128];

	SKIP_WHITESPACE(p);

	/* Get the address */
	if((tp=strrchr(p,'<'))!=NULL)
		tp++;
	else
		tp=p;
	SKIP_WHITESPACE(tp);
	sprintf(addr,"%.*s",addr_len,tp);
	truncstr(addr,">( ");

	SAFECOPY(tmp,p);
	p=tmp;
	/* Get the "name" (if possible) */
	if((tp=strchr(p,'('))!=NULL) {			/* name in parenthesis? */
		p=tp+1;
		tp=strchr(p,')');
	} else if((tp=strchr(p,'"'))!=NULL) {	/* name in quotes? */
		p=tp+1;
		tp=strchr(p,'"');
	} else if(*p=='<') {					/* address in brackets? */
		p++;
		tp=strchr(p,'>');
	} else									/* name, then address in brackets */
		tp=strchr(p,'<');
	if(tp) *tp=0;
	sprintf(name,"%.*s",name_len,p);
	truncsp(name);
}

/* Decode quoted-printable content-transfer-encoded text */
/* Ignores (strips) unsupported ctrl chars and non-ASCII chars */
/* Does not enforce 76 char line length limit */
static char* qp_decode(char* buf)
{
	uchar*	p=(uchar*)buf;
	uchar*	dest=p;

	for(;;p++) {
		if(*p==0) {
			*dest++='\r';
			*dest++='\n';
			break;
		}
		if(*p==' ' || (*p>='!' && *p<='~' && *p!='=') || *p=='\t')
			*dest++=*p;
		else if(*p=='=') {
			p++;
			if(*p==0) 	/* soft link break */
				break;
			if(isxdigit(*p) && isxdigit(*(p+1))) {
				char hex[3];
				hex[0]=*p;
				hex[1]=*(p+1);
				hex[2]=0;
				/* ToDo: what about encoded NULs and the like? */
				*dest++=(uchar)strtoul(hex,NULL,16);
				p++;
			} else {	/* bad encoding */
				*dest++='=';
				*dest++=*p;
			}
		}
	}
	*dest=0;
	return buf;
}

static BOOL checktag(scfg_t *scfg, char *tag, uint usernum)
{
	char	fname[MAX_PATH+1];

	if(tag==NULL)
		return(FALSE);
	SAFEPRINTF2(fname,"%suser/%04d.smtpblock",scfg->data_dir,usernum);
	return(findstr(tag, fname));
}

static BOOL smtp_splittag(char *in, char **name, char **tag)
{
	char	*last;

	if(in==NULL)
		return(FALSE);

	*name=strtok_r(in, "#", &last);
	if(*name) {
		*tag=strtok_r(NULL, "", &last);
		return(TRUE);
	}
	return(FALSE);
}

static uint smtp_matchuser(scfg_t *scfg, char *str, BOOL aliases, BOOL datdupe)
{
	char	*user=strdup(str);
	char	*name;
	char	*tag=NULL;
	uint	usernum=0;

	if(!user)
		return(0);

	if(!smtp_splittag(user, &name, &tag))
		goto end;

	if(datdupe)
		usernum=userdatdupe(scfg, 0, U_NAME, LEN_NAME, name, /* del */FALSE, /* next */FALSE);
	else
		usernum=matchuser(scfg, name, aliases);

	if(!usernum)
		goto end;

	if(checktag(scfg, tag, usernum))
		usernum=UINT_MAX;

end:
	free(user);
	return(usernum);
}

static void smtp_thread(void* arg)
{
	int			i,j;
	int			rd;
	char		str[512];
	char		tmp[128];
	char		path[MAX_PATH+1];
	char		value[INI_MAX_VALUE_LEN];
	str_list_t	sec_list;
	char*		section;
	char		buf[1024],*p,*tp,*cp;
	char		hdrfield[512];
	char		alias_buf[128];
	char		name_alias_buf[128];
	char		reverse_path[128];
	char		date[64];
	char		qwkid[32];
	char		rcpt_to[128];
	char		rcpt_name[128];
	char		rcpt_addr[128];
	char		sender[128];
	char		sender_ext[128];
	char		sender_addr[128];
	char		hello_name[128];
	char		user_name[128];
	char		user_pass[128];
	char		relay_list[MAX_PATH+1];
	char		domain_list[MAX_PATH+1];
	char		spam_bait[MAX_PATH+1];
	BOOL		spam_bait_result=FALSE;
	char		spam_block[MAX_PATH+1];
	char		spam_block_exempt[MAX_PATH+1];
	char		host_name[128];
	char		host_ip[64];
	char		dnsbl[256];
	char		dnsbl_ip[64];
	char*		telegram_buf;
	char*		msgbuf;
	char		challenge[256];
	char		response[128];
	char		secret[64];
	char		md5_data[384];
	uchar		digest[MD5_DIGEST_SIZE];
	char		dest_host[128];
	char*		errmsg;
	ushort		dest_port;
	socklen_t	addr_len;
	ushort		hfield_type;
	ushort		nettype;
	ushort		agent;
	uint		usernum;
	ulong		lines=0;
	ulong		hdr_lines=0;
	ulong		hdr_len=0;
	ulong		length;
	ulong		badcmds=0;
	ulong		login_attempts;
	ulong		waiting;
	BOOL		esmtp=FALSE;
	BOOL		telegram=FALSE;
	BOOL		forward=FALSE;
	BOOL		no_forward=FALSE;
	BOOL		auth_login;
	BOOL		routed=FALSE;
	BOOL		dnsbl_recvhdr;
	BOOL		msg_handled;
	uint		subnum=INVALID_SUB;
	FILE*		msgtxt=NULL;
	char		msgtxt_fname[MAX_PATH+1];
	char		newtxt_fname[MAX_PATH+1];
	char		logtxt_fname[MAX_PATH+1];
	FILE*		rcptlst;
	char		rcptlst_fname[MAX_PATH+1];
	ushort		rcpt_count=0;
	FILE*		proc_err;
	char		proc_err_fname[MAX_PATH+1];
	char		session_id[MAX_PATH+1];
	FILE*		spy=NULL;
	SOCKET		socket;
	HOSTENT*	host;
	int			smb_error;
	smb_t		smb;
	smb_t		spam;
	smbmsg_t	msg;
	smbmsg_t	newmsg;
	user_t		user;
	user_t		relay_user;
	node_t		node;
	client_t	client;
	smtp_t		smtp=*(smtp_t*)arg;
	SOCKADDR_IN server_addr;
	IN_ADDR		dnsbl_result;
	BOOL*		mailproc_to_match;
	int			mailproc_match;
	JSRuntime*	js_runtime=NULL;
	JSContext*	js_cx=NULL;
	JSObject*	js_glob=NULL;
	int32		js_result;
	struct mailproc*	mailproc;

	enum {
			 SMTP_STATE_INITIAL
			,SMTP_STATE_HELO
			,SMTP_STATE_MAIL_FROM
			,SMTP_STATE_RCPT_TO
			,SMTP_STATE_DATA_HEADER
			,SMTP_STATE_DATA_BODY

	} state = SMTP_STATE_INITIAL;

	enum {
			 SMTP_CMD_NONE
			,SMTP_CMD_MAIL
			,SMTP_CMD_SEND
			,SMTP_CMD_SOML
			,SMTP_CMD_SAML

	} cmd = SMTP_CMD_NONE;

	enum {
			 ENCODING_NONE
			,ENCODING_BASE64
			,ENCODING_QUOTED_PRINTABLE
	} content_encoding = ENCODING_NONE;

	SetThreadName("SMTP");
	thread_up(TRUE /* setuid */);

	free(arg);

	socket=smtp.socket;

	lprintf(LOG_DEBUG,"%04d SMTP Session thread started", socket);

#ifdef _WIN32
	if(startup->inbound_sound[0] && !(startup->options&MAIL_OPT_MUTE)) 
		PlaySound(startup->inbound_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif

	addr_len=sizeof(server_addr);
	if((i=getsockname(socket, (struct sockaddr *)&server_addr,&addr_len))!=0) {
		lprintf(LOG_CRIT,"%04d !SMTP ERROR %d (%d) getting address/port"
			,socket, i, ERROR_VALUE);
		sockprintf(socket,sys_error);
		mail_close_socket(socket);
		thread_down();
		return;
	} 

	if((mailproc_to_match=malloc(sizeof(BOOL)*mailproc_count))==NULL) {
		lprintf(LOG_CRIT,"%04d !SMTP ERROR allocating memory for mailproc_to_match", socket);
		sockprintf(socket,sys_error);
		mail_close_socket(socket);
		thread_down();
		return;
	} 
	memset(mailproc_to_match,FALSE,sizeof(BOOL)*mailproc_count);

	memset(&smb,0,sizeof(smb));
	memset(&msg,0,sizeof(msg));
	memset(&spam,0,sizeof(spam));
	memset(&user,0,sizeof(user));
	memset(&relay_user,0,sizeof(relay_user));

	SAFECOPY(host_ip,inet_ntoa(smtp.client_addr.sin_addr));

	lprintf(LOG_INFO,"%04d SMTP Connection accepted on port %u from: %s port %u"
		,socket, BE_INT16(server_addr.sin_port), host_ip, ntohs(smtp.client_addr.sin_port));

	if(startup->options&MAIL_OPT_NO_HOST_LOOKUP)
		host=NULL;
	else
		host=gethostbyaddr ((char *)&smtp.client_addr.sin_addr
			,sizeof(smtp.client_addr.sin_addr),AF_INET);

	if(host!=NULL && host->h_name!=NULL)
		SAFECOPY(host_name,host->h_name);
	else
		strcpy(host_name,"<no name>");

	if(!(startup->options&MAIL_OPT_NO_HOST_LOOKUP))
		lprintf(LOG_INFO,"%04d SMTP Hostname: %s", socket, host_name);

	protected_uint32_adjust(&active_clients, 1);
	update_clients();

	SAFECOPY(hello_name,host_name);

	SAFEPRINTF(spam_bait,"%sspambait.cfg",scfg.ctrl_dir);
	SAFEPRINTF(spam_block,"%sspamblock.cfg",scfg.ctrl_dir);
	SAFEPRINTF(spam_block_exempt,"%sspamblock_exempt.cfg",scfg.ctrl_dir);

	if(smtp.client_addr.sin_addr.s_addr==server_addr.sin_addr.s_addr 
		|| smtp.client_addr.sin_addr.s_addr==htonl(IPv4_LOCALHOST)) {
		/* local connection */
		dnsbl_result.s_addr=0;
	} else {
		if(trashcan(&scfg,host_ip,"ip") 
			|| (findstr(host_ip,spam_block) && !findstr(host_ip,spam_block_exempt))) {
			lprintf(LOG_NOTICE,"%04d !SMTP CLIENT IP ADDRESS BLOCKED: %s (%u total)"
				,socket, host_ip, ++stats.sessions_refused);
			sockprintf(socket,"550 CLIENT IP ADDRESS BLOCKED: %s", host_ip);
			mail_close_socket(socket);
			thread_down();
			protected_uint32_adjust(&active_clients, -1);
			update_clients();
			free(mailproc_to_match);
			return;
		}

		if(trashcan(&scfg,host_name,"host") 
			|| (findstr(host_name,spam_block) && !findstr(host_name,spam_block_exempt))) {
			lprintf(LOG_NOTICE,"%04d !SMTP CLIENT HOSTNAME BLOCKED: %s (%u total)"
				,socket, host_name, ++stats.sessions_refused);
			sockprintf(socket,"550 CLIENT HOSTNAME BLOCKED: %s", host_name);
			mail_close_socket(socket);
			thread_down();
			protected_uint32_adjust(&active_clients, -1);
			update_clients();
			free(mailproc_to_match);
			return;
		}

		/*  SPAM Filters (mail-abuse.org) */
		dnsbl_result.s_addr = dns_blacklisted(socket,smtp.client_addr.sin_addr,host_name,dnsbl,dnsbl_ip);
		if(dnsbl_result.s_addr) {
			lprintf(LOG_NOTICE,"%04d SMTP BLACKLISTED SERVER on %s: %s [%s] = %s"
				,socket, dnsbl, host_name, dnsbl_ip, inet_ntoa(dnsbl_result));
			if(startup->options&MAIL_OPT_DNSBL_REFUSE) {
				SAFEPRINTF2(str,"Listed on %s as %s", dnsbl, inet_ntoa(dnsbl_result));
				spamlog(&scfg, "SMTP", "SESSION REFUSED", str, host_name, dnsbl_ip, NULL, NULL);
				sockprintf(socket
					,"550 Mail from %s refused due to listing at %s"
					,dnsbl_ip, dnsbl);
				mail_close_socket(socket);
				lprintf(LOG_NOTICE,"%04d !SMTP REFUSED SESSION from blacklisted server (%u total)"
					,socket, ++stats.sessions_refused);
				thread_down();
				protected_uint32_adjust(&active_clients, -1);
				update_clients();
				free(mailproc_to_match);
				return;
			}
		}
	}

	SAFEPRINTF(smb.file,"%smail",scfg.data_dir);
	if(smb_islocked(&smb)) {
		lprintf(LOG_CRIT,"%04d !SMTP MAIL BASE LOCKED: %s"
			,socket, smb.last_error);
		sockprintf(socket,sys_unavail);
		mail_close_socket(socket);
		thread_down();
		protected_uint32_adjust(&active_clients, -1);
		update_clients();
		free(mailproc_to_match);
		return;
	}
	SAFEPRINTF(spam.file,"%sspam",scfg.data_dir);
	spam.retry_time=scfg.smb_retry_time;
	spam.subnum=INVALID_SUB;

	srand((unsigned int)(time(NULL) ^ (time_t)GetCurrentThreadId()));	/* seed random number generator */
	rand();	/* throw-away first result */
	SAFEPRINTF4(session_id,"%x%x%x%lx",getpid(),socket,rand(),clock());
	lprintf(LOG_DEBUG,"%04d SMTP Session ID=%s", socket, session_id);
	SAFEPRINTF2(msgtxt_fname,"%sSBBS_SMTP.%s.msg", scfg.temp_dir, session_id);
	SAFEPRINTF2(newtxt_fname,"%sSBBS_SMTP.%s.new", scfg.temp_dir, session_id);
	SAFEPRINTF2(logtxt_fname,"%sSBBS_SMTP.%s.log", scfg.temp_dir, session_id);
	SAFEPRINTF2(rcptlst_fname,"%sSBBS_SMTP.%s.lst", scfg.temp_dir, session_id);
	rcptlst=fopen(rcptlst_fname,"w+");
	if(rcptlst==NULL) {
		lprintf(LOG_CRIT,"%04d !SMTP ERROR %d creating recipient list: %s"
			,socket, errno, rcptlst_fname);
		sockprintf(socket,sys_error);
		mail_close_socket(socket);
		thread_down();
		protected_uint32_adjust(&active_clients, -1);
		update_clients();
		free(mailproc_to_match);
		return;
	}

	if(trashcan(&scfg,host_name,"smtpspy") 
		|| trashcan(&scfg,host_ip,"smtpspy")) {
		SAFEPRINTF(str,"%ssmtpspy.txt", scfg.logs_dir);
		spy=fopen(str,"a");
	}

	/* Initialize client display */
	client.size=sizeof(client);
	client.time=time32(NULL);
	SAFECOPY(client.addr,host_ip);
	SAFECOPY(client.host,host_name);
	client.port=ntohs(smtp.client_addr.sin_port);
	client.protocol="SMTP";
	client.user="<unknown>";
	client_on(socket,&client,FALSE /* update */);

	SAFEPRINTF(str,"SMTP: %s",host_ip);
	status(str);

	if(startup->login_attempt_throttle
		&& (login_attempts=loginAttempts(startup->login_attempt_list, &smtp.client_addr)) > 1) {
		lprintf(LOG_DEBUG,"%04d SMTP Throttling suspicious connection from: %s (%u login attempts)"
			,socket, inet_ntoa(smtp.client_addr.sin_addr), login_attempts);
		mswait(login_attempts*startup->login_attempt_throttle);
	}

	/* SMTP session active: */

	sockprintf(socket,"220 %s Synchronet SMTP Server %s-%s Ready"
		,startup->host_name,revision,PLATFORM_DESC);
	while(1) {
		rd = sockreadline(socket, buf, sizeof(buf));
		if(rd<0) 
			break;
		truncsp(buf);
		if(spy!=NULL)
			fprintf(spy,"%s\n",buf);
		if(relay_user.number==0 && dnsbl_result.s_addr && startup->options&MAIL_OPT_DNSBL_THROTTLE)
			mswait(DNSBL_THROTTLE_VALUE);
		if(state>=SMTP_STATE_DATA_HEADER) {
			if(!strcmp(buf,".")) {

				state=SMTP_STATE_HELO;	/* RESET state machine here in case of error */
				cmd=SMTP_CMD_NONE;

				if(msgtxt==NULL) {
					lprintf(LOG_ERR,"%04d !SMTP NO MESSAGE TEXT FILE POINTER?", socket);
					sockprintf(socket,"554 No message text");
					continue;
				}

				if(ftell(msgtxt)<1) {
					lprintf(LOG_ERR,"%04d !SMTP INVALID MESSAGE LENGTH: %ld (%lu lines)"
						, socket, ftell(msgtxt), lines);
					sockprintf(socket,"554 No message text");
					continue;
				}

				lprintf(LOG_INFO,"%04d SMTP End of message (body: %lu lines, %lu bytes, header: %lu lines, %lu bytes)"
					, socket, lines, ftell(msgtxt)-hdr_len, hdr_lines, hdr_len);

				if(!socket_check(socket, NULL, NULL, 0)) {
					lprintf(LOG_WARNING,"%04d !SMTP sender disconnected (premature evacuation)", socket);
					continue;
				}

				stats.msgs_received++;

				/* Twit-listing (sender's name and e-mail addresses) here */
				SAFEPRINTF(path,"%stwitlist.cfg",scfg.ctrl_dir);
				if(fexist(path) && (findstr(sender,path) || findstr(sender_addr,path))) {
					lprintf(LOG_NOTICE,"%04d !SMTP FILTERING TWIT-LISTED SENDER: %s <%s> (%u total)"
						,socket, sender, sender_addr, ++stats.msgs_refused);
					SAFEPRINTF2(tmp,"Twit-listed sender: %s <%s>", sender, sender_addr);
					spamlog(&scfg, "SMTP", "REFUSED", tmp, host_name, host_ip, rcpt_addr, reverse_path);
					sockprintf(socket, "554 Sender not allowed.");
					continue;
				}

				if(telegram==TRUE) {		/* Telegram */
					const char* head="\1n\1h\1cInstant Message\1n from \1h\1y";
					const char* tail="\1n:\r\n\1h";
					rewind(msgtxt);
					length=filelength(fileno(msgtxt));
					
					p=strchr(sender_addr,'@');
					if(p==NULL || resolve_ip(p+1)!=smtp.client_addr.sin_addr.s_addr) 
						/* Append real IP and hostname if different */
						safe_snprintf(str,sizeof(str),"%s%s\r\n\1w[\1n%s\1h] (\1n%s\1h)%s"
							,head,sender_addr,host_ip,host_name,tail);
					else
						safe_snprintf(str,sizeof(str),"%s%s%s",head,sender_addr,tail);
					
					if((telegram_buf=(char*)malloc(length+strlen(str)+1))==NULL) {
						lprintf(LOG_CRIT,"%04d !SMTP ERROR allocating %lu bytes of memory for telegram from %s"
							,socket,length+strlen(str)+1,sender_addr);
						sockprintf(socket, insuf_stor);
						continue; 
					}
					strcpy(telegram_buf,str);	/* can't use SAFECOPY here */
					if(fread(telegram_buf+strlen(str),1,length,msgtxt)!=length) {
						lprintf(LOG_ERR,"%04d !SMTP ERROR reading %lu bytes from telegram file"
							,socket,length);
						sockprintf(socket, insuf_stor);
						free(telegram_buf);
						continue; 
					}
					telegram_buf[length+strlen(str)]=0;	/* Need ASCIIZ */

					/* Send telegram to users */
					sec_list=iniReadSectionList(rcptlst,NULL);	/* Each section is a recipient */
					for(rcpt_count=0; sec_list!=NULL
						&& sec_list[rcpt_count]!=NULL 
						&& (startup->max_recipients==0 || rcpt_count<startup->max_recipients); rcpt_count++) {

						section=sec_list[rcpt_count];

						SAFECOPY(rcpt_to,iniReadString(rcptlst,section	,smb_hfieldtype(RECIPIENT),"unknown",value));
						usernum=iniReadInteger(rcptlst,section				,smb_hfieldtype(RECIPIENTEXT),0);
						SAFECOPY(rcpt_addr,iniReadString(rcptlst,section	,smb_hfieldtype(RECIPIENTNETADDR),rcpt_to,value));

						if((i=putsmsg(&scfg,usernum,telegram_buf))==0)
							lprintf(LOG_INFO,"%04d SMTP Created telegram (%ld/%u bytes) from %s to %s <%s>"
								,socket, length, strlen(telegram_buf), sender_addr, rcpt_to, rcpt_addr);
						else
							lprintf(LOG_ERR,"%04d !SMTP ERROR %d creating telegram from %s to %s <%s>"
								,socket, i, sender_addr, rcpt_to, rcpt_addr);
					}
					iniFreeStringList(sec_list);
					free(telegram_buf);
					sockprintf(socket,ok_rsp);
					telegram=FALSE;
					continue;
				}

				fclose(msgtxt), msgtxt=NULL;
				fclose(rcptlst), rcptlst=NULL;

				/* External Mail Processing here */
				mailproc=NULL;
				msg_handled=FALSE;
				if(mailproc_count) {
					SAFEPRINTF2(proc_err_fname,"%sSBBS_SMTP.%s.err", scfg.temp_dir, session_id);
					remove(proc_err_fname);

					for(i=0;i<mailproc_count;i++) {
	
						mailproc=&mailproc_list[i];
						if(mailproc->disabled)
							continue;

						if(!mailproc->process_dnsbl && dnsbl_result.s_addr)
							continue;

						if(!mailproc->process_spam && spam_bait_result)
							continue;

						if(!chk_ar(&scfg,mailproc->ar,&relay_user,&client))
							continue;

						if(mailproc->to!=NULL && !mailproc_to_match[i])
							continue;

						if(mailproc->from!=NULL 
							&& !findstr_in_list(sender_addr, mailproc->from))
							continue;

						if(!mailproc->passthru)
							msg_handled=TRUE;

						mailcmdstr(mailproc->cmdline
							,msgtxt_fname, newtxt_fname, logtxt_fname
							,rcptlst_fname, proc_err_fname
							,host_name, host_ip, relay_user.number
							,rcpt_addr
							,sender, sender_addr, reverse_path, str);
						lprintf(LOG_INFO,"%04d SMTP Executing external mail processor: %s"
							,socket, mailproc->name);

						if(mailproc->native) {
							lprintf(LOG_DEBUG,"%04d SMTP Executing external command: %s"
								,socket, str);
							if((j=system(str))!=0) {
								lprintf(LOG_NOTICE,"%04d SMTP system(%s) returned %d (errno: %d)"
									,socket, str, j, errno);
								if(mailproc->ignore_on_error) {
									lprintf(LOG_WARNING,"%04d !SMTP IGNORED MAIL due to mail processor (%s) error: %d"
										,socket, mailproc->name, j);
									msg_handled=TRUE;
								}
							}
						} else {  /* JavaScript */
							if(!js_mailproc(socket, &client, &relay_user
								,mailproc
								,str /* cmdline */
								,msgtxt_fname, newtxt_fname, logtxt_fname
								,rcpt_addr
								,rcptlst_fname, proc_err_fname
								,sender, sender_addr, reverse_path, hello_name, &js_result
								,&js_runtime, &js_cx, &js_glob
								,"SMTP") || js_result!=0) {
#if 0 /* calling exit() in a script causes js_mailproc to return FALSE */
								lprintf(LOG_NOTICE,"%04d !SMTP JavaScript mailproc command (%s) failed (returned: %d)"
									,socket, str, js_result);
								if(mailproc->ignore_on_error) {
									lprintf(LOG_WARNING,"%04d !SMTP IGNORED MAIL due to mail processor (%s) failure"
										,socket, mailproc->name);
									msg_handled=TRUE;
								}
#endif
							}
						}
						if(flength(proc_err_fname)>0)
							break;
						if(!fexist(msgtxt_fname) || !fexist(rcptlst_fname))
							break;
					}
					if(flength(proc_err_fname)>0 
						&& (proc_err=fopen(proc_err_fname,"r"))!=NULL) {
						while(!feof(proc_err)) {
							int n;
							if(!fgets(str,sizeof(str),proc_err))
								break;
							truncsp(str);
							lprintf(LOG_WARNING,"%04d !SMTP External mail processor (%s) error: %s"
								,socket, mailproc->name, str);
							n=atoi(str);
							if(n>=100 && n<1000)
								sockprintf(socket,"%s", str);
							else
								sockprintf(socket,"554%c%s"
									,ftell(proc_err)<filelength(fileno(proc_err)) ? '-' : ' '
									,str);
						}
						fclose(proc_err);
						msg_handled=TRUE;
					}
					else if(!fexist(msgtxt_fname) || !fexist(rcptlst_fname)) {
						lprintf(LOG_NOTICE,"%04d SMTP External mail processor (%s) removed %s file"
							,socket
							,mailproc->name
							,fexist(msgtxt_fname)==FALSE ? "message text" : "recipient list");
						sockprintf(socket,ok_rsp);
						msg_handled=TRUE;
					}
					else if(msg_handled)
						sockprintf(socket,ok_rsp);
					remove(proc_err_fname);	/* Remove error file here */
				}

				/* Re-open files */
				/* We must do this before continuing for handled msgs */
				/* to prevent freopen(NULL) and orphaned temp files */
				if((rcptlst=fopen(rcptlst_fname,fexist(rcptlst_fname) ? "r":"w+"))==NULL) {
					lprintf(LOG_ERR,"%04d !SMTP ERROR %d re-opening recipient list: %s"
						,socket, errno, rcptlst_fname);
					if(!msg_handled)
						sockprintf(socket,sys_error);
					continue;
				}
			
				if(!msg_handled && subnum==INVALID_SUB && iniReadSectionCount(rcptlst,NULL) < 1) {
					lprintf(LOG_DEBUG,"%04d SMTP No recipients in recipient list file (message handled by external mail processor?)"
						,socket);
					sockprintf(socket,ok_rsp);
					msg_handled=TRUE;
				}
				if(msg_handled) {
					if(mailproc!=NULL)
						lprintf(LOG_NOTICE,"%04d SMTP Message handled by external mail processor (%s, %u total)"
							,socket, mailproc->name, ++mailproc->handled);
					continue;
				}

				/* If mailproc has written new message text to .new file, use that instead of .msg */
				if(flength(newtxt_fname) > 0) {
					remove(msgtxt_fname);
					SAFECOPY(msgtxt_fname, newtxt_fname);
				} else
					remove(newtxt_fname);

				if((msgtxt=fopen(msgtxt_fname,"rb"))==NULL) {
					lprintf(LOG_ERR,"%04d !SMTP ERROR %d re-opening message file: %s"
						,socket, errno, msgtxt_fname);
					sockprintf(socket,sys_error);
					continue;
				}

				/* Initialize message header */
				smb_freemsgmem(&msg);
				memset(&msg,0,sizeof(smbmsg_t));		

				/* Parse message header here */
				hfield_type=UNKNOWN;
				smb_error=SMB_SUCCESS; /* no SMB error */
				errmsg=insuf_stor;
				while(!feof(msgtxt)) {
					char field[32];

					if(!fgets(buf,sizeof(buf),msgtxt))
						break;
					truncsp(buf);
					if(buf[0]==0)	/* blank line marks end of header */
						break;

					if((p=get_header_field(buf, field, sizeof(field)))!=NULL) {
						if(stricmp(field, "SUBJECT")==0) {
							/* SPAM Filtering/Logging */
							if(relay_user.number==0) {
								if(trashcan(&scfg,p,"subject")) {
									lprintf(LOG_NOTICE,"%04d !SMTP BLOCKED SUBJECT (%s) from: %s (%u total)"
										,socket, p, reverse_path, ++stats.msgs_refused);
									SAFEPRINTF2(tmp,"Blocked subject (%s) from: %s"
										,p, reverse_path);
									spamlog(&scfg, "SMTP", "REFUSED"
										,tmp, host_name, host_ip, rcpt_addr, reverse_path);
									errmsg="554 Subject not allowed.";
									smb_error=SMB_FAILURE;
									break;
								}
								if(dnsbl_result.s_addr && startup->dnsbl_tag[0] && !(startup->options&MAIL_OPT_DNSBL_IGNORE)) {
									safe_snprintf(str,sizeof(str),"%.*s: %.*s"
										,(int)sizeof(str)/2, startup->dnsbl_tag
										,(int)sizeof(str)/2, p);
									p=str;
									lprintf(LOG_NOTICE,"%04d SMTP TAGGED MAIL SUBJECT from blacklisted server with: %s"
										,socket, startup->dnsbl_tag);
								}
							}
							smb_hfield_str(&msg, hfield_type=SUBJECT, p);
							continue;
						}
						if(relay_user.number==0	&& stricmp(field, "FROM")==0
							&& !chk_email_addr(socket,p,host_name,host_ip,rcpt_addr,reverse_path,"FROM")) {
							errmsg="554 Sender not allowed.";
							smb_error=SMB_FAILURE;
							break;
						}
						if(relay_user.number==0 && stricmp(field, "TO")==0 && !spam_bait_result
							&& !chk_email_addr(socket,p,host_name,host_ip,rcpt_addr,reverse_path,"TO")) {
							errmsg="550 Unknown user.";
							smb_error=SMB_FAILURE;
							break;
						}
					}
					if((smb_error=parse_header_field((char*)buf,&msg,&hfield_type))!=SMB_SUCCESS) {
						if(smb_error==SMB_ERR_HDR_LEN)
							lprintf(LOG_WARNING,"%04d !SMTP MESSAGE HEADER EXCEEDS %u BYTES"
								,socket, SMB_MAX_HDR_LEN);
						else
							lprintf(LOG_ERR,"%04d !SMTP ERROR %d adding header field: %s"
								,socket, smb_error, buf);
						break;
					}
				}
				if(smb_error!=SMB_SUCCESS) {	/* SMB Error */
					sockprintf(socket, errmsg);
					stats.msgs_refused++;
					continue;
				}
				if((p=smb_get_hfield(&msg, RFC822TO, NULL))!=NULL) {
					parse_mail_address(p
						,rcpt_name	,sizeof(rcpt_name)-1
						,rcpt_addr	,sizeof(rcpt_addr)-1);
				}
				if((p=smb_get_hfield(&msg, RFC822FROM, NULL))!=NULL) {
					parse_mail_address(p 
						,sender		,sizeof(sender)-1
						,sender_addr,sizeof(sender_addr)-1);
				}
				dnsbl_recvhdr=FALSE;
				if(startup->options&MAIL_OPT_DNSBL_CHKRECVHDRS)  {
					for(i=0;!dnsbl_result.s_addr && i<msg.total_hfields;i++)  {
						if(msg.hfield[i].type == SMTPRECEIVED)  {
							if(chk_received_hdr(socket,msg.hfield_dat[i],&dnsbl_result,dnsbl,dnsbl_ip)) {
								dnsbl_recvhdr=TRUE;
								break;
							}
						}
					}
				}
				if(relay_user.number==0 && dnsbl_result.s_addr && !(startup->options&MAIL_OPT_DNSBL_IGNORE)) {
					/* tag message as spam */
					if(startup->dnsbl_hdr[0]) {
						safe_snprintf(str,sizeof(str),"%s: %s is listed on %s as %s"
							,startup->dnsbl_hdr, dnsbl_ip
							,dnsbl, inet_ntoa(dnsbl_result));
						smb_hfield_str(&msg, RFC822HEADER, str);
						lprintf(LOG_NOTICE,"%04d SMTP TAGGED MAIL HEADER from blacklisted server with: %s"
							,socket, startup->dnsbl_hdr);
					}
					if(startup->dnsbl_hdr[0] || startup->dnsbl_tag[0]) {
						SAFEPRINTF2(str,"Listed on %s as %s", dnsbl, inet_ntoa(dnsbl_result));
						spamlog(&scfg, "SMTP", "TAGGED", str, host_name, dnsbl_ip, rcpt_addr, reverse_path);
					}
				}
				if(dnsbl_recvhdr)			/* DNSBL-listed IP found in Received header? */
					dnsbl_result.s_addr=0;	/* Reset DNSBL look-up result between messages */

				if(sender[0]==0) {
					lprintf(LOG_WARNING,"%04d !SMTP MISSING mail header 'FROM' field (%u total)"
						,socket, ++stats.msgs_refused);
					sockprintf(socket, "554 Mail header missing 'FROM' field");
					subnum=INVALID_SUB;
					continue;
				}
				if(relay_user.number) {
					SAFEPRINTF(str,"%u",relay_user.number);
					smb_hfield_str(&msg, SENDEREXT, str);
				}
				if(relay_user.number && subnum!=INVALID_SUB) {
					nettype=NET_NONE;
					smb_hfield_str(&msg, SENDER, relay_user.alias);
				} else {
					nettype=NET_INTERNET;
					smb_hfield_str(&msg, SENDER, sender);
					smb_hfield(&msg, SENDERNETTYPE, sizeof(nettype), &nettype);
					smb_hfield_str(&msg, SENDERNETADDR, sender_addr);
				}
				smb_hfield_str(&msg, SMTPREVERSEPATH, reverse_path);
				if(msg.subj==NULL)
					smb_hfield(&msg, SUBJECT, 0, NULL);

				length=filelength(fileno(msgtxt))-ftell(msgtxt);

				if(startup->max_msg_size && length>startup->max_msg_size) {
					lprintf(LOG_WARNING,"%04d !SMTP Message size (%lu) exceeds maximum: %lu bytes"
						,socket,length,startup->max_msg_size);
					sockprintf(socket, "552 Message size (%lu) exceeds maximum: %lu bytes"
						,length,startup->max_msg_size);
					subnum=INVALID_SUB;
					stats.msgs_refused++;
					continue;
				}

				if((msgbuf=(char*)malloc(length+1))==NULL) {
					lprintf(LOG_CRIT,"%04d !SMTP ERROR allocating %d bytes of memory"
						,socket,length+1);
					sockprintf(socket, insuf_stor);
					subnum=INVALID_SUB;
					continue;
				}
				fread(msgbuf,length,1,msgtxt);
				msgbuf[length]=0;	/* ASCIIZ */

				/* Do external JavaScript processing here? */

				if(subnum!=INVALID_SUB) {	/* Message Base */
					uint reason;
					if(relay_user.number==0)
						memset(&relay_user,0,sizeof(relay_user));

					if(!can_user_post(&scfg,subnum,&relay_user,&client,&reason)) {
						lprintf(LOG_WARNING,"%04d !SMTP %s (user #%u) cannot post on %s (reason: %u)"
							,socket, sender_addr, relay_user.number
							,scfg.sub[subnum]->sname, reason);
						sockprintf(socket,"550 Insufficient access");
						subnum=INVALID_SUB;
						stats.msgs_refused++;
						continue;
					}

					if(rcpt_name[0]==0)
						strcpy(rcpt_name,"All");
					smb_hfield_str(&msg, RECIPIENT, rcpt_name);

					smb.subnum=subnum;
					if((i=savemsg(&scfg, &smb, &msg, &client, startup->host_name, msgbuf))!=SMB_SUCCESS) {
						lprintf(LOG_WARNING,"%04d !SMTP ERROR %d (%s) saving message"
							,socket,i,smb.last_error);
						sockprintf(socket, "452 ERROR %d (%s) saving message"
							,i,smb.last_error);
					} else {
						lprintf(LOG_INFO,"%04d SMTP %s posted a message on %s"
							,socket, sender_addr, scfg.sub[subnum]->sname);
						sockprintf(socket,ok_rsp);
						if(relay_user.number != 0)
							user_posted_msg(&scfg, &relay_user, 1);
						signal_smtp_sem();
					}
					free(msgbuf);
					smb_close(&smb);
					subnum=INVALID_SUB;
					continue;
				}

				/* Create/check hashes of known SPAM */
				{
					hash_t**	hashes;
					BOOL		is_spam=spam_bait_result;
					long		sources=SMB_HASH_SOURCE_SPAM;

					if((dnsbl_recvhdr || dnsbl_result.s_addr) && startup->options&MAIL_OPT_DNSBL_SPAMHASH)
						is_spam=TRUE;

					if(msg.subj==NULL || strlen(msg.subj) < SPAM_HASH_SUBJECT_MIN_LEN)
						sources&=~(1<<SMB_HASH_SOURCE_SUBJECT);
					lprintf(LOG_DEBUG,"%04d SMTP Calculating message hashes (sources=%lx, msglen=%u)"
						,socket, sources, strlen(msgbuf));
					if((hashes=smb_msghashes(&msg, (uchar*)msgbuf, sources)) != NULL) {
						hash_t	found;

						for(i=0;hashes[i];i++)
							lprintf(LOG_DEBUG,"%04d SMTP Message %s crc32=%lx flags=%lx length=%u"
								,socket, smb_hashsourcetype(hashes[i]->source)
								,hashes[i]->crc32, hashes[i]->flags, hashes[i]->length);

						if((i=smb_findhash(&spam, hashes, &found, sources, /* Mark: */TRUE))==SMB_SUCCESS) {
							SAFEPRINTF3(str,"%s (%s) found in SPAM database (added on %s)"
								,smb_hashsourcetype(found.source)
								,smb_hashsource(&msg,found.source)
								,timestr(&scfg,found.time,tmp)
								);
							lprintf(LOG_NOTICE,"%04d SMTP Message %s", socket, str);
							if(!is_spam) {
								spamlog(&scfg, "SMTP", "IGNORED"
									,str, host_name, host_ip, rcpt_addr, reverse_path);
								is_spam=TRUE;
							}
						} else if(i!=SMB_ERR_NOT_FOUND)
							lprintf(LOG_ERR,"%04d !SMTP ERROR %d (%s) opening SPAM database"
								,socket, i, spam.last_error);
						
						if(is_spam) {
							size_t	n,total=0;
							for(n=0;hashes[n]!=NULL;n++)
								if(!(hashes[n]->flags&SMB_HASH_MARKED)) {
									lprintf(LOG_INFO,"%04d SMTP Adding message %s (%s) to SPAM database"
										,socket
										,smb_hashsourcetype(hashes[n]->source)
										,smb_hashsource(&msg,hashes[n]->source)
										);
									total++;
								}
							if(total) {
								lprintf(LOG_DEBUG,"%04d SMTP Adding %u message hashes to SPAM database", socket, total);
								smb_addhashes(&spam, hashes, /* skip_marked: */TRUE);
							}
							if(i!=SMB_SUCCESS && !spam_bait_result && (dnsbl_recvhdr || dnsbl_result.s_addr))
								is_spam=FALSE;
						}
						smb_close_hash(&spam);

						smb_freehashes(hashes);
					} else
						lprintf(LOG_ERR,"%04d SMTP smb_msghashes returned NULL", socket);

					if(is_spam || ((startup->options&MAIL_OPT_DNSBL_IGNORE) && (dnsbl_recvhdr || dnsbl_result.s_addr))) {
						free(msgbuf);
						if(is_spam)
							lprintf(LOG_NOTICE,"%04d !SMTP IGNORED SPAM MESSAGE (%u total)"
								,socket, ++stats.msgs_ignored);
						else {
							SAFEPRINTF2(str,"Listed on %s as %s", dnsbl, inet_ntoa(dnsbl_result));
							lprintf(LOG_NOTICE,"%04d !SMTP IGNORED MAIL from server: %s (%u total)"
								,socket, str, ++stats.msgs_ignored);
							spamlog(&scfg, "SMTP", "IGNORED"
								,str, host_name, dnsbl_ip, rcpt_addr, reverse_path);
						}
						/* pretend we received it */
						sockprintf(socket,ok_rsp);
						subnum=INVALID_SUB;
						continue;
					}
				}

				/* E-mail */
				smb.subnum=INVALID_SUB;
				/* creates message data, but no header or index records (since msg.to==NULL) */
				i=savemsg(&scfg, &smb, &msg, &client, startup->host_name, msgbuf);
				free(msgbuf);
				if(i!=SMB_SUCCESS) {
					smb_close(&smb);
					lprintf(LOG_CRIT,"%04d !SMTP ERROR %d (%s) saving message"
						,socket,i,smb.last_error);
					sockprintf(socket, "452 ERROR %d (%s) saving message"
						,i,smb.last_error);
					continue;
				}

				lprintf(LOG_DEBUG,"%04d SMTP Recipient name: '%s'", socket, rcpt_name);

				sec_list=iniReadSectionList(rcptlst,NULL);	/* Each section is a recipient */
				for(rcpt_count=0; sec_list!=NULL
					&& sec_list[rcpt_count]!=NULL 
					&& (startup->max_recipients==0 || rcpt_count<startup->max_recipients); rcpt_count++) {
				
					section=sec_list[rcpt_count];

					SAFECOPY(rcpt_to,iniReadString(rcptlst,section	,smb_hfieldtype(RECIPIENT),"unknown",value));
					usernum=iniReadInteger(rcptlst,section				,smb_hfieldtype(RECIPIENTEXT),0);
					agent=iniReadShortInt(rcptlst,section				,smb_hfieldtype(RECIPIENTAGENT),AGENT_PERSON);
					nettype=iniReadShortInt(rcptlst,section				,smb_hfieldtype(RECIPIENTNETTYPE),NET_NONE);
					SAFEPRINTF(str,"#%u",usernum);
					SAFECOPY(rcpt_addr,iniReadString(rcptlst,section	,smb_hfieldtype(RECIPIENTNETADDR),str,value));

					if(nettype==NET_NONE /* Local destination */ && usernum==0) {
						lprintf(LOG_ERR,"%04d !SMTP can't deliver mail to user #0"
							,socket);
						break;
					}

					if((i=smb_copymsgmem(&smb,&newmsg,&msg))!=SMB_SUCCESS) {
						lprintf(LOG_ERR,"%04d !SMTP ERROR %d (%s) copying message"
							,socket, i, smb.last_error);
						break;
					}

					snprintf(hdrfield,sizeof(hdrfield),
						"from %s (%s [%s])\r\n"
						"          by %s [%s] (%s %s-%s) with %s\r\n"
						"          for %s; %s\r\n"
						"          (envelope-from %s)"
						,host_name,hello_name,host_ip
						,startup->host_name,inet_ntoa(server_addr.sin_addr)
						,server_name
						,revision,PLATFORM_DESC
						,esmtp ? "ESMTP" : "SMTP"
						,rcpt_to,msgdate(msg.hdr.when_imported,date)
						,reverse_path);
					smb_hfield_add_str(&newmsg, SMTPRECEIVED, hdrfield, /* insert: */TRUE);

					smb_hfield_str(&newmsg, RECIPIENT, rcpt_name);

					if(usernum && nettype!=NET_INTERNET) {	/* Local destination or QWKnet routed */
						/* This is required for fixsmb to be able to rebuild the index */
						SAFEPRINTF(str,"%u",usernum);
						smb_hfield_str(&newmsg, RECIPIENTEXT, str);
					}
					if(nettype!=NET_NONE) {
						smb_hfield(&newmsg, RECIPIENTNETTYPE, sizeof(nettype), &nettype);
						smb_hfield_str(&newmsg, RECIPIENTNETADDR, rcpt_addr);
					}
					if(agent!=newmsg.to_agent)
						smb_hfield(&newmsg, RECIPIENTAGENT, sizeof(agent), &agent);

					i=smb_addmsghdr(&smb,&newmsg,SMB_SELFPACK);
					smb_freemsgmem(&newmsg);
					if(i!=SMB_SUCCESS) {
						lprintf(LOG_ERR,"%04d !SMTP ERROR %d (%s) adding message header"
							,socket, i, smb.last_error);
						break;
					}
					sender_ext[0]=0;
					if(msg.from_ext!=NULL)
						SAFEPRINTF(sender_ext," #%s",msg.from_ext);
					lprintf(LOG_INFO,"%04d SMTP Created message #%ld from %s%s [%s] to %s [%s]"
						,socket, newmsg.hdr.number, sender, sender_ext, smb_netaddrstr(&msg.from_net,tmp), rcpt_name, rcpt_addr);
					if(relay_user.number!=0)
						user_sent_email(&scfg, &relay_user, 1, usernum==1);
					if(!(startup->options&MAIL_OPT_NO_NOTIFY) && usernum) {
						if(newmsg.idx.to)
							for(i=1;i<=scfg.sys_nodes;i++) {
								getnodedat(&scfg, i, &node, 0);
								if(node.useron==usernum
									&& (node.status==NODE_INUSE || node.status==NODE_QUIET))
									break;
							}
						if(!newmsg.idx.to || i<=scfg.sys_nodes) {
							safe_snprintf(str,sizeof(str)
								,"\7\1n\1hOn %.24s\r\n\1m%s \1n\1msent you e-mail from: "
								"\1h%s\1n\r\n"
								,timestr(&scfg,newmsg.hdr.when_imported.time,tmp)
								,sender,sender_addr);
							if(!newmsg.idx.to) {	/* Forwarding */
								strcat(str,"\1mand it was automatically forwarded to: \1h");
								strcat(str,rcpt_addr);
								strcat(str,"\1n\r\n");
							}
							putsmsg(&scfg, usernum, str);
						}
					}
				}
				iniFreeStringList(sec_list);
				if(rcpt_count<1) {
					smb_freemsg_dfields(&smb,&msg,SMB_ALL_REFS);
					sockprintf(socket, insuf_stor);
				}
				else {
					if(rcpt_count>1)
						smb_incmsg_dfields(&smb,&msg,(ushort)(rcpt_count-1));
					sockprintf(socket,ok_rsp);
					signal_smtp_sem();
				}
#if 0 /* This shouldn't be necessary here */
				smb_close_da(&smb);
#endif
				smb_close(&smb);
				continue;
			}
			if(buf[0]==0 && state==SMTP_STATE_DATA_HEADER) {	
				state=SMTP_STATE_DATA_BODY;	/* Null line separates header and body */
				lines=0;
				if(msgtxt!=NULL) {
					fprintf(msgtxt, "\r\n");
					hdr_len=ftell(msgtxt);
				}
				continue;
			}
			if(state==SMTP_STATE_DATA_BODY) {
				p=buf;
				if(*p=='.') p++;	/* Transparency (RFC821 4.5.2) */
				if(msgtxt!=NULL) {
					switch(content_encoding) {
						case ENCODING_BASE64:
							{
								char	decode_buf[sizeof(buf)];

								if(b64_decode(decode_buf, sizeof(decode_buf), p, strlen(p))<0)
									fprintf(msgtxt,"\r\n!Base64 decode error: %s\r\n", p);
								else
									fputs(decode_buf, msgtxt);
							}
							break;
						case ENCODING_QUOTED_PRINTABLE:
							fputs(qp_decode(p), msgtxt);
							break;
						default:
							fprintf(msgtxt, "%s\r\n", p);
							break;
					}
				}
				lines++;
				/* release time-slices every x lines */
				if(startup->lines_per_yield &&
					!(lines%startup->lines_per_yield))	
					YIELD();
				continue;
			}
			/* RFC822 Header parsing */
			if(startup->options&MAIL_OPT_DEBUG_RX_HEADER)
				lprintf(LOG_DEBUG,"%04d SMTP %s",socket, buf);

			{
				char field[32];

				if((p=get_header_field(buf, field, sizeof(field)))!=NULL) {
					if(stricmp(field, "FROM")==0) {
						parse_mail_address(p
							,sender,		sizeof(sender)-1
							,sender_addr,	sizeof(sender_addr)-1);
					}
					else if(stricmp(field,"CONTENT-TRANSFER-ENCODING")==0) {
						lprintf(LOG_INFO,"%04d SMTP %s = %s", socket, field, p);
						if(stricmp(p,"base64")==0)
							content_encoding=ENCODING_BASE64;
						else if(stricmp(p,"quoted-printable")==0)
							content_encoding=ENCODING_QUOTED_PRINTABLE;
						else {	/* Other (e.g. 7bit, 8bit, binary) */
							content_encoding=ENCODING_NONE;
							if(msgtxt!=NULL) 
								fprintf(msgtxt, "%s\r\n", buf);
						}
						hdr_lines++;
						continue;
					}
				}
			}

			if(msgtxt!=NULL) 
				fprintf(msgtxt, "%s\r\n", buf);
			hdr_lines++;
			continue;
		}
		strip_ctrl(buf, buf);
		lprintf(LOG_DEBUG,"%04d SMTP RX: %s", socket, buf);
		if(!strnicmp(buf,"HELO",4)) {
			p=buf+4;
			SKIP_WHITESPACE(p);
			SAFECOPY(hello_name,p);
			sockprintf(socket,"250 %s",startup->host_name);
			esmtp=FALSE;
			state=SMTP_STATE_HELO;
			cmd=SMTP_CMD_NONE;
			telegram=FALSE;
			subnum=INVALID_SUB;
			continue;
		}
		if(!strnicmp(buf,"EHLO",4)) {
			p=buf+4;
			SKIP_WHITESPACE(p);
			SAFECOPY(hello_name,p);
			sockprintf(socket,"250-%s",startup->host_name);
			sockprintf(socket,"250-AUTH PLAIN LOGIN CRAM-MD5");
			sockprintf(socket,"250-SEND");
			sockprintf(socket,"250-SOML");
			sockprintf(socket,"250-SAML");
			sockprintf(socket,"250-8BITMIME");
			sockprintf(socket,"250 SIZE %lu", startup->max_msg_size);
			esmtp=TRUE;
			state=SMTP_STATE_HELO;
			cmd=SMTP_CMD_NONE;
			telegram=FALSE;
			subnum=INVALID_SUB;
			continue;
		}
		if((auth_login=(stricmp(buf,"AUTH LOGIN")==0))==TRUE 
			|| strnicmp(buf,"AUTH PLAIN",10)==0) {
			if(auth_login) {
				sockprintf(socket,"334 VXNlcm5hbWU6");	/* Base64-encoded "Username:" */
				if((rd=sockreadline(socket, buf, sizeof(buf)))<1) {
					sockprintf(socket,badarg_rsp);
					continue;
				}
				if(startup->options&MAIL_OPT_DEBUG_RX_RSP) 
					lprintf(LOG_DEBUG,"%04d RX: %s",socket,buf);
				if(b64_decode(user_name,sizeof(user_name),buf,rd)<1) {
					sockprintf(socket,badarg_rsp);
					continue;
				}
				sockprintf(socket,"334 UGFzc3dvcmQ6");	/* Base64-encoded "Password:" */
				if((rd=sockreadline(socket, buf, sizeof(buf)))<1) {
					sockprintf(socket,badarg_rsp);
					continue;
				}
				if(startup->options&MAIL_OPT_DEBUG_RX_RSP) 
					lprintf(LOG_DEBUG,"%04d RX: %s",socket,buf);
				if(b64_decode(user_pass,sizeof(user_pass),buf,rd)<1) {
					sockprintf(socket,badarg_rsp);
					continue;
				}
			} else {	/* AUTH PLAIN b64(<username>\0<user-id>\0<password>) */
				p=buf+10;
				SKIP_WHITESPACE(p);
				if(*p==0) {
					sockprintf(socket,badarg_rsp);
					continue;
				}
				ZERO_VAR(tmp);
				if(b64_decode(tmp,sizeof(tmp),p,strlen(p))<1) {
					sockprintf(socket,badarg_rsp);
					continue;
				}
				p=tmp;
				while(*p) p++;	/* skip username */
				p++;			/* skip NULL */
				if(*p==0) {
					sockprintf(socket,badarg_rsp);
					continue;
				}
				SAFECOPY(user_name,p);
				while(*p) p++;	/* skip user-id */
				p++;			/* skip NULL */
				if(*p==0) {
					sockprintf(socket,badarg_rsp);
					continue;
				}
				SAFECOPY(user_pass,p);
			}

			if((relay_user.number=matchuser(&scfg,user_name,FALSE))==0) {
				if(scfg.sys_misc&SM_ECHO_PW)
					lprintf(LOG_WARNING,"%04d !SMTP UNKNOWN USER: %s (password: %s)"
						,socket, user_name, user_pass);
				else
					lprintf(LOG_WARNING,"%04d !SMTP UNKNOWN USER: %s"
						,socket, user_name);
				badlogin(socket, client.protocol, badauth_rsp, user_name, user_pass, host_name, &smtp.client_addr);
				break;
			}
			if((i=getuserdat(&scfg, &relay_user))!=0) {
				lprintf(LOG_ERR,"%04d !SMTP ERROR %d getting data on user (%s)"
					,socket, i, user_name);
				badlogin(socket, client.protocol, badauth_rsp, NULL, NULL, NULL, NULL);
				break;
			}
			if(relay_user.misc&(DELETED|INACTIVE)) {
				lprintf(LOG_WARNING,"%04d !SMTP DELETED or INACTIVE user #%u (%s)"
					,socket, relay_user.number, user_name);
				badlogin(socket, client.protocol, badauth_rsp, NULL, NULL, NULL, NULL);
				break;
			}
			if(stricmp(user_pass,relay_user.pass)) {
				if(scfg.sys_misc&SM_ECHO_PW)
					lprintf(LOG_WARNING,"%04d !SMTP FAILED Password attempt for user %s: '%s' expected '%s'"
						,socket, user_name, user_pass, relay_user.pass);
				else
					lprintf(LOG_WARNING,"%04d !SMTP FAILED Password attempt for user %s"
						,socket, user_name);
				badlogin(socket, client.protocol, badauth_rsp, user_name, user_pass, host_name, &smtp.client_addr);
				break;
			}

			if(relay_user.pass[0])
				loginSuccess(startup->login_attempt_list, &smtp.client_addr);

			/* Update client display */
			client.user=relay_user.alias;
			client_on(socket,&client,TRUE /* update */);

			lprintf(LOG_INFO,"%04d SMTP %s authenticated using %s authentication"
				,socket,relay_user.alias,auth_login ? "LOGIN" : "PLAIN");
			sockprintf(socket,auth_ok);
			continue;
		}
		if(!stricmp(buf,"AUTH CRAM-MD5")) {
			safe_snprintf(challenge,sizeof(challenge),"<%x%x%lx%lx@%s>"
				,rand(),socket,(ulong)time(NULL),clock(),startup->host_name);
#if 0
			lprintf(LOG_DEBUG,"%04d SMTP CRAM-MD5 challenge: %s"
				,socket,challenge);
#endif
			b64_encode(str,sizeof(str),challenge,0);
			sockprintf(socket,"334 %s",str);
			if((rd=sockreadline(socket, buf, sizeof(buf)))<1) {
				sockprintf(socket,badarg_rsp);
				continue;
			}
			if(startup->options&MAIL_OPT_DEBUG_RX_RSP) 
				lprintf(LOG_DEBUG,"%04d RX: %s",socket,buf);

			if(b64_decode(response,sizeof(response),buf,rd)<1) {
				sockprintf(socket,badarg_rsp);
				continue;
			}
#if 0
			lprintf(LOG_DEBUG,"%04d SMTP CRAM-MD5 response: %s"
				,socket,response);
#endif
			if((p=strrchr(response,' '))!=NULL)
				*(p++)=0;
			else
				p=response;
			SAFECOPY(user_name,response);
			if((relay_user.number=matchuser(&scfg,user_name,FALSE))==0) {
				lprintf(LOG_WARNING,"%04d !SMTP UNKNOWN USER: %s"
					,socket, user_name);
				badlogin(socket, client.protocol, badauth_rsp, user_name, user_pass, host_name, &smtp.client_addr);
				break;
			}
			if((i=getuserdat(&scfg, &relay_user))!=0) {
				lprintf(LOG_ERR,"%04d !SMTP ERROR %d getting data on user (%s)"
					,socket, i, user_name);
				badlogin(socket, client.protocol, badauth_rsp, NULL, NULL, NULL, NULL);
				break;
			}
			if(relay_user.misc&(DELETED|INACTIVE)) {
				lprintf(LOG_WARNING,"%04d !SMTP DELETED or INACTIVE user #%u (%s)"
					,socket, relay_user.number, user_name);
				badlogin(socket, client.protocol, badauth_rsp, NULL, NULL, NULL, NULL);
				break;
			}
			/* Calculate correct response */
			memset(secret,0,sizeof(secret));
			SAFECOPY(secret,relay_user.pass);
			strlwr(secret);	/* this is case sensitive, so convert to lowercase first */
			for(i=0;i<sizeof(secret);i++)
				md5_data[i]=secret[i]^0x36;	/* ipad */
			strcpy(md5_data+i,challenge);
			MD5_calc(digest,md5_data,sizeof(secret)+strlen(challenge));
			for(i=0;i<sizeof(secret);i++)
				md5_data[i]=secret[i]^0x5c;	/* opad */
			memcpy(md5_data+i,digest,sizeof(digest));
			MD5_calc(digest,md5_data,sizeof(secret)+sizeof(digest));
			MD5_hex((BYTE*)str,digest);
			if(strcmp(p,str)) {
				lprintf(LOG_WARNING,"%04d !SMTP %s FAILED CRAM-MD5 authentication"
					,socket,relay_user.alias);
#if 0
				lprintf(LOG_DEBUG,"%04d !SMTP calc digest: %s"
					,socket,str);
				lprintf(LOG_DEBUG,"%04d !SMTP resp digest: %s"
					,socket,p);
#endif
				badlogin(socket, client.protocol, badauth_rsp, user_name, p, host_name, &smtp.client_addr);
				break;
			}

			if(relay_user.pass[0])
				loginSuccess(startup->login_attempt_list, &smtp.client_addr);

			/* Update client display */
			client.user=relay_user.alias;
			client_on(socket,&client,TRUE /* update */);

			lprintf(LOG_INFO,"%04d SMTP %s authenticated using CRAM-MD5 authentication"
				,socket,relay_user.alias);
			sockprintf(socket,auth_ok);
			continue;
		}
		if(!strnicmp(buf,"AUTH",4)) {
			sockprintf(socket,"504 Unrecognized authentication type.");
			continue;
		}
		if(!stricmp(buf,"QUIT")) {
			sockprintf(socket,"221 %s Service closing transmission channel",startup->host_name);
			break;
		} 
		if(!stricmp(buf,"NOOP")) {
			sockprintf(socket, ok_rsp);
			badcmds=0;
			continue;
		}
		if(state<SMTP_STATE_HELO) {
			/* RFC 821 4.1.1 "The first command in a session must be the HELO command." */
			lprintf(LOG_WARNING,"%04d !SMTP MISSING 'HELO' command",socket);
			sockprintf(socket, badseq_rsp);
			continue;
		}
		if(!stricmp(buf,"TURN")) {
			sockprintf(socket,"502 command not supported");
			badcmds=0;
			continue;
		}
		if(!stricmp(buf,"RSET")) {
			smb_freemsgmem(&msg);
			memset(&msg,0,sizeof(smbmsg_t));		/* Initialize message header */
			reverse_path[0]=0;
			state=SMTP_STATE_HELO;
			cmd=SMTP_CMD_NONE;
			telegram=FALSE;
			subnum=INVALID_SUB;
			spam_bait_result=FALSE;

			/* reset recipient list */
			if((rcptlst=freopen(rcptlst_fname,"w+",rcptlst))==NULL) {
				lprintf(LOG_ERR,"%04d !SMTP ERROR %d re-opening %s"
					,socket, errno, rcptlst_fname);
				sockprintf(socket,sys_error);
				break;
			}
			rcpt_count=0;
			content_encoding=ENCODING_NONE;

			memset(mailproc_to_match,FALSE,sizeof(BOOL)*mailproc_count);

			sockprintf(socket,ok_rsp);
			badcmds=0;
			lprintf(LOG_INFO,"%04d SMTP Session reset",socket);
			continue;
		}
		if(!strnicmp(buf,"MAIL FROM:",10)
			|| !strnicmp(buf,"SEND FROM:",10)	/* Send a Message (Telegram) to a local ONLINE user */
			|| !strnicmp(buf,"SOML FROM:",10)	/* Send OR Mail a Message to a local user */
			|| !strnicmp(buf,"SAML FROM:",10)	/* Send AND Mail a Message to a local user */
			) {
			p=buf+10;
			if(relay_user.number==0
				&& !chk_email_addr(socket,p,host_name,host_ip,NULL,NULL,"REVERSE PATH")) {
				sockprintf(socket, "554 Sender not allowed.");
				stats.msgs_refused++;
				break;
			}
			SKIP_WHITESPACE(p);
			SAFECOPY(reverse_path,p);
			if((p=strchr(reverse_path,' '))!=NULL)	/* Truncate "<user@domain> KEYWORD=VALUE" to just "<user@domain>" per RFC 1869 */
				*p=0;

			/* If MAIL FROM address is in dnsbl_exempt.cfg, clear DNSBL results */
			if(dnsbl_result.s_addr && email_addr_is_exempt(reverse_path)) {
				lprintf(LOG_INFO,"%04d SMTP Ignoring DNSBL results for exempt sender: %s"
					,socket,reverse_path);
				dnsbl_result.s_addr=0;
			}

			/* Update client display */
			if(relay_user.number==0) {
				client.user=reverse_path;
				client_on(socket,&client,TRUE /* update */);
			}

			/* Setup state */
			state=SMTP_STATE_MAIL_FROM;
			if(!strnicmp(buf,"MAIL FROM:",10))
				cmd=SMTP_CMD_MAIL;
			else if(!strnicmp(buf,"SEND FROM:",10))
				cmd=SMTP_CMD_SEND;
			else if(!strnicmp(buf,"SOML FROM:",10))
				cmd=SMTP_CMD_SOML;
			else if(!strnicmp(buf,"SAML FROM:",10))
				cmd=SMTP_CMD_SAML;

			/* reset recipient list */
			if((rcptlst=freopen(rcptlst_fname,"w+",rcptlst))==NULL) {
				lprintf(LOG_ERR,"%04d !SMTP ERROR %d re-opening %s"
					,socket, errno, rcptlst_fname);
				sockprintf(socket,sys_error);
				break;
			}
			rcpt_count=0;
			content_encoding=ENCODING_NONE;
			memset(mailproc_to_match,FALSE,sizeof(BOOL)*mailproc_count);
			sockprintf(socket,ok_rsp);
			badcmds=0;
			continue;
		}

#if 0	/* No one uses this command */
		if(!strnicmp(buf,"VRFY",4)) {
			p=buf+4;
			SKIP_WHITESPACE(p);
			if(*p==0) {
				sockprintf(socket,"550 No user specified.");
				continue;
			}
#endif

		/* Add to Recipient list */
		if(!strnicmp(buf,"RCPT TO:",8)) {

			if(state<SMTP_STATE_MAIL_FROM) {
				lprintf(LOG_WARNING,"%04d !SMTP MISSING 'MAIL' command",socket);
				sockprintf(socket, badseq_rsp);
				continue;
			}

			p=buf+8;
			SKIP_WHITESPACE(p);
			SAFECOPY(rcpt_to,p);
			SAFECOPY(str,p);
			p=strrchr(str,'<');
			if(p==NULL)
				p=str;
			else
				p++;

			truncstr(str,">");	/* was truncating at space too */

			routed=FALSE;
			forward=FALSE;
			no_forward=FALSE;
			if(!strnicmp(p,FORWARD,strlen(FORWARD))) {
				forward=TRUE;		/* force forward to user's netmail address */
				p+=strlen(FORWARD);
			}
			if(!strnicmp(p,NO_FORWARD,strlen(NO_FORWARD))) {
				no_forward=TRUE;	/* do not forward to user's netmail address */
				p+=strlen(NO_FORWARD);
			}

			if(*p==0) {
				lprintf(LOG_NOTICE,"%04d !SMTP NO RECIPIENT SPECIFIED"
					,socket);
				sockprintf(socket, "500 No recipient specified");
				continue;
			}

			rcpt_name[0]=0;
			SAFECOPY(rcpt_addr,p);

			/* Check recipient counter */
			if(startup->max_recipients) {
				if(rcpt_count>=startup->max_recipients) {
					lprintf(LOG_NOTICE,"%04d !SMTP MAXIMUM RECIPIENTS (%d) REACHED"
						,socket, startup->max_recipients);
					SAFEPRINTF(tmp,"Maximum recipient count (%d)",startup->max_recipients);
					spamlog(&scfg, "SMTP", "REFUSED", tmp
						,host_name, host_ip, rcpt_addr, reverse_path);
					sockprintf(socket, "452 Too many recipients");
					stats.msgs_refused++;
					continue;
				}
				if(relay_user.number!=0 && !(relay_user.exempt&FLAG('M'))
					&& rcpt_count+(waiting=getmail(&scfg,relay_user.number,/* sent: */TRUE)) > startup->max_recipients) {
					lprintf(LOG_NOTICE,"%04d !SMTP MAXIMUM PENDING SENT EMAILS (%u) REACHED for User #%u (%s)"
						,socket, waiting, relay_user.number, relay_user.alias);
					sockprintf(socket, "452 Too many pending emails sent");
					stats.msgs_refused++;
					continue;
				}
			}

			if(relay_user.number && (relay_user.etoday+rcpt_count) >= scfg.level_emailperday[relay_user.level]
				&& !(relay_user.exempt&FLAG('M'))) {
				lprintf(LOG_NOTICE,"%04d !SMTP EMAILS PER DAY LIMIT (%u) REACHED FOR USER #%u (%s)"
					,socket, scfg.level_emailperday[relay_user.level], relay_user.number, relay_user.alias);
				SAFEPRINTF2(tmp,"Maximum emails per day (%u) for %s"
					,scfg.level_emailperday[relay_user.level], relay_user.alias);
				spamlog(&scfg, "SMTP", "REFUSED", tmp
					,host_name, host_ip, rcpt_addr, reverse_path);
				sockprintf(socket, "452 Too many emails today");
				stats.msgs_refused++;
				continue;
			}
				
			/* Check for SPAM bait recipient */
			if((spam_bait_result=findstr(rcpt_addr,spam_bait))==TRUE) {
				char	reason[256];
				SAFEPRINTF(reason,"SPAM BAIT (%s) taken", rcpt_addr);
				lprintf(LOG_NOTICE,"%04d SMTP %s by: %s"
					,socket, reason, reverse_path);
				if(relay_user.number==0) {
					strcpy(tmp,"IGNORED");
					if(dnsbl_result.s_addr==0						/* Don't double-filter */
						&& !findstr(host_name,spam_block_exempt)
						&& !findstr(host_ip,spam_block_exempt))	{ 
						lprintf(LOG_NOTICE,"%04d !BLOCKING IP ADDRESS: %s in %s", socket, host_ip, spam_block);
						filter_ip(&scfg, "SMTP", reason, host_name, host_ip, reverse_path, spam_block);
						strcat(tmp," and BLOCKED");
					}
					spamlog(&scfg, "SMTP", tmp, "Attempted recipient in SPAM BAIT list"
						,host_name, host_ip, rcpt_addr, reverse_path);
					dnsbl_result.s_addr=0;
				}
				sockprintf(socket,ok_rsp);
				state=SMTP_STATE_RCPT_TO;
				continue;
			}

			/* Check for blocked recipients */
			if(relay_user.number==0
				&& !chk_email_addr(socket,rcpt_addr,host_name,host_ip,rcpt_addr,reverse_path,"RECIPIENT")) {
				sockprintf(socket, "550 Unknown User: %s", rcpt_to);
				stats.msgs_refused++;
				continue;
			}

			if(relay_user.number==0 && dnsbl_result.s_addr && startup->options&MAIL_OPT_DNSBL_BADUSER) {
				lprintf(LOG_NOTICE,"%04d !SMTP REFUSED MAIL from blacklisted server (%u total)"
					,socket, ++stats.sessions_refused);
				SAFEPRINTF2(str,"Listed on %s as %s", dnsbl, inet_ntoa(dnsbl_result));
				spamlog(&scfg, "SMTP", "REFUSED", str, host_name, host_ip, rcpt_addr, reverse_path);
				sockprintf(socket
					,"550 Mail from %s refused due to listing at %s"
					,host_ip, dnsbl);
				break;
			}

			if(spy==NULL 
				&& (trashcan(&scfg,reverse_path,"smtpspy")
					|| trashcan(&scfg,rcpt_addr,"smtpspy"))) {
				SAFEPRINTF(path,"%ssmtpspy.txt", scfg.logs_dir);
				spy=fopen(path,"a");
			}

			/* Check for full address aliases */
			p=alias(&scfg,p,alias_buf);
			if(p==alias_buf) 
				lprintf(LOG_DEBUG,"%04d SMTP ADDRESS ALIAS: %s (for %s)"
					,socket,p,rcpt_addr);

			tp=strrchr(p,'@');
			if(cmd==SMTP_CMD_MAIL && tp!=NULL) {
				
				/* RELAY */
				dest_port=server_addr.sin_port;
				SAFECOPY(dest_host,tp+1);
				cp=strrchr(dest_host,':');
				if(cp!=NULL) {
					*cp=0;
					dest_port=atoi(cp+1);
				}
				SAFEPRINTF(domain_list,"%sdomains.cfg",scfg.ctrl_dir);
				if((stricmp(dest_host,scfg.sys_inetaddr)!=0
						&& stricmp(dest_host,startup->host_name)!=0
						&& resolve_ip(dest_host)!=server_addr.sin_addr.s_addr
						&& findstr(dest_host,domain_list)==FALSE)
					|| dest_port!=server_addr.sin_port) {

					SAFEPRINTF(relay_list,"%srelay.cfg",scfg.ctrl_dir);
					if(relay_user.number==0 /* not authenticated, search for IP */
						&& startup->options&MAIL_OPT_SMTP_AUTH_VIA_IP) { 
						relay_user.number=userdatdupe(&scfg, 0, U_NOTE, LEN_NOTE, host_ip, /* del */FALSE, /* next */FALSE);
						if(relay_user.number) {
							getuserdat(&scfg,&relay_user);
							if(relay_user.laston < time(NULL)-(60*60))	/* logon in past hour? */
								relay_user.number=0;
						}
					} else
						getuserdat(&scfg,&relay_user);
					if(p!=alias_buf /* forced relay by alias */ &&
						(!(startup->options&MAIL_OPT_ALLOW_RELAY)
							|| relay_user.number==0
							|| relay_user.rest&(FLAG('G')|FLAG('M'))) &&
						!findstr(host_name,relay_list) && 
						!findstr(host_ip,relay_list)) {
						lprintf(LOG_WARNING,"%04d !SMTP ILLEGAL RELAY ATTEMPT from %s [%s] to %s"
							,socket, reverse_path, host_ip, p);
						SAFEPRINTF(tmp,"Relay attempt to: %s", p);
						spamlog(&scfg, "SMTP", "REFUSED", tmp, host_name, host_ip, rcpt_addr, reverse_path);
						if(startup->options&MAIL_OPT_ALLOW_RELAY)
							sockprintf(socket, "553 Relaying through this server "
							"requires authentication.  "
							"Please authenticate before sending.");
						else {
							sockprintf(socket, "550 Relay not allowed.");
							stats.msgs_refused++;
						}
						break;
					}

					if(relay_user.number==0)
						SAFECOPY(relay_user.alias,"Unknown User");

					lprintf(LOG_INFO,"%04d SMTP %s relaying to external mail service: %s"
						,socket, relay_user.alias, tp+1);

					fprintf(rcptlst,"[%u]\n",rcpt_count++);
					fprintf(rcptlst,"%s=%s\n",smb_hfieldtype(RECIPIENT),rcpt_addr);
					fprintf(rcptlst,"%s=%u\n",smb_hfieldtype(RECIPIENTNETTYPE),NET_INTERNET);
					fprintf(rcptlst,"%s=%s\n",smb_hfieldtype(RECIPIENTNETADDR),p);

					sockprintf(socket,ok_rsp);
					state=SMTP_STATE_RCPT_TO;
					continue;
				}
			}
			if(tp!=NULL)
				*tp=0;	/* truncate at '@' */

			tp=strchr(p,'!');	/* Routed QWKnet mail in <qwkid!user@host> format */
			if(tp!=NULL) {
				*(tp++)=0;
				SKIP_CHAR(tp,'"');				/* Skip '"' */
				truncstr(tp,"\"");				/* Strip '"' */
				SAFECOPY(rcpt_addr,tp);
				routed=TRUE;
			}

			FIND_ALPHANUMERIC(p);				/* Skip '<' or '"' */
			truncstr(p,"\"");	

			p=alias(&scfg,p,name_alias_buf);
			if(p==name_alias_buf) 
				lprintf(LOG_DEBUG,"%04d SMTP NAME ALIAS: %s (for %s)"
					,socket,p,rcpt_addr);
		
			/* Check if message is to be processed by an external mail processor */
			for(i=0;i<mailproc_count;i++) {

				if(!mailproc_list[i].process_dnsbl && dnsbl_result.s_addr)
					continue;

				if(!mailproc_list[i].process_spam && spam_bait_result)
					continue;

				if(!chk_ar(&scfg,mailproc_list[i].ar,&relay_user,&client))
					continue;

				if(findstr_in_list(p, mailproc_list[i].to)) {
					mailproc_to_match[i]=TRUE;
					break;
				}
			}
			mailproc_match=i;

			if(!strnicmp(p,"sub:",4)) {		/* Post on a sub-board */
				p+=4;
				for(i=0;i<scfg.total_subs;i++)
					if(!stricmp(p,scfg.sub[i]->code))
						break;
				if(i>=scfg.total_subs) {
					lprintf(LOG_NOTICE,"%04d !SMTP UNKNOWN SUB-BOARD: %s", socket, p);
					sockprintf(socket, "550 Unknown sub-board: %s", p);
					continue;
				}
				subnum=i;
				sockprintf(socket,ok_rsp);
				state=SMTP_STATE_RCPT_TO;
				rcpt_count++;
				continue;
			}

			/* destined for a (non-passthru) external mail processor */
			if(mailproc_match<mailproc_count) {
				fprintf(rcptlst,"[%u]\n",rcpt_count++);
				fprintf(rcptlst,"%s=%s\n",smb_hfieldtype(RECIPIENT),rcpt_addr);
#if 0	/* should we fall-through to the sysop account? */
				fprintf(rcptlst,"%s=%u\n",smb_hfieldtype(RECIPIENTEXT),1);
#endif
				lprintf(LOG_INFO,"%04d SMTP Routing mail for %s to External Mail Processor: %s"
					,socket, rcpt_addr, mailproc_list[mailproc_match].name);
				sockprintf(socket,ok_rsp);
				state=SMTP_STATE_RCPT_TO;
				continue;
			}

			usernum=0;	/* unknown user at this point */

			if(routed) {
				SAFECOPY(qwkid,p);
				truncstr(qwkid,"/");
				/* Search QWKnet hub-IDs for route destination */
				for(i=0;i<scfg.total_qhubs;i++) {
					if(!stricmp(qwkid,scfg.qhub[i]->id))
						break;
				}
				if(i<scfg.total_qhubs) {	/* found matching QWKnet Hub */

					lprintf(LOG_INFO,"%04d SMTP Routing mail for %s <%s> to QWKnet Hub: %s"
						,socket, rcpt_addr, p, scfg.qhub[i]->id);

					fprintf(rcptlst,"[%u]\n",rcpt_count++);
					fprintf(rcptlst,"%s=%s\n",smb_hfieldtype(RECIPIENT),rcpt_addr);
					fprintf(rcptlst,"%s=%u\n",smb_hfieldtype(RECIPIENTNETTYPE),NET_QWK);
					fprintf(rcptlst,"%s=%s\n",smb_hfieldtype(RECIPIENTNETADDR),p);

					sockprintf(socket,ok_rsp);
					state=SMTP_STATE_RCPT_TO;
					continue;
				}
			}

			if((p==alias_buf || p==name_alias_buf || startup->options&MAIL_OPT_ALLOW_RX_BY_NUMBER)
				&& isdigit(*p)) {
				usernum=atoi(p);			/* RX by user number */
				/* verify usernum */
				username(&scfg,usernum,str);
				if(!str[0] || !stricmp(str,"DELETED USER"))
					usernum=0;
				p=str;
			} else {
				/* RX by "user alias", "user.alias" or "user_alias" */
				usernum=smtp_matchuser(&scfg,p,startup->options&MAIL_OPT_ALLOW_SYSOP_ALIASES,FALSE);	

				if(!usernum) { /* RX by "real name", "real.name", or "sysop.alias" */
					
					/* convert "user.name" to "user name" */
					SAFECOPY(rcpt_name,p);
					for(tp=rcpt_name;*tp;tp++)	
						if(*tp=='.') *tp=' ';

					if(!stricmp(p,scfg.sys_op) || !stricmp(rcpt_name,scfg.sys_op))
						usernum=1;			/* RX by "sysop.alias" */

					if(!usernum && scfg.msg_misc&MM_REALNAME)	/* RX by "real name" */
						usernum=smtp_matchuser(&scfg, p, FALSE, TRUE);	

					if(!usernum && scfg.msg_misc&MM_REALNAME)	/* RX by "real.name" */
						usernum=smtp_matchuser(&scfg, rcpt_name, FALSE, TRUE);	
				}
			}
			if(!usernum && startup->default_user[0]) {
				usernum=matchuser(&scfg,startup->default_user,TRUE /* sysop_alias */);
				if(usernum)
					lprintf(LOG_INFO,"%04d SMTP Forwarding mail for UNKNOWN USER to default user: %s #%u"
						,socket,startup->default_user,usernum);
				else
					lprintf(LOG_WARNING,"%04d !SMTP UNKNOWN DEFAULT USER: %s"
						,socket,startup->default_user);
			}

			if(usernum==UINT_MAX) {
				lprintf(LOG_INFO,"%04d SMTP Blocked tag: %s", socket, rcpt_to);
				sockprintf(socket, "550 Unknown User: %s", rcpt_to);
				continue;
			}
			if(!usernum) {
				lprintf(LOG_WARNING,"%04d !SMTP UNKNOWN USER: %s", socket, rcpt_to);
				sockprintf(socket, "550 Unknown User: %s", rcpt_to);
				continue;
			}
			user.number=usernum;
			if((i=getuserdat(&scfg, &user))!=0) {
				lprintf(LOG_ERR,"%04d !SMTP ERROR %d getting data on user #%u (%s)"
					,socket, i, usernum, p);
				sockprintf(socket, "550 Unknown User: %s", rcpt_to);
				continue;
			}
			if(user.misc&(DELETED|INACTIVE)) {
				lprintf(LOG_WARNING,"%04d !SMTP DELETED or INACTIVE user #%u (%s)"
					,socket, usernum, p);
				sockprintf(socket, "550 Unknown User: %s", rcpt_to);
				continue;
			}
			if(cmd==SMTP_CMD_MAIL) {
				if((user.rest&FLAG('M')) && relay_user.number==0) {
					lprintf(LOG_NOTICE,"%04d !SMTP M-restricted user #u (%s) cannot receive unauthenticated SMTP mail"
						,socket, user.number, user.alias);
					sockprintf(socket, "550 Closed mailbox: %s", rcpt_to);
					stats.msgs_refused++;
					continue;
				}
				if(startup->max_msgs_waiting && !(user.exempt&FLAG('W')) 
					&& (waiting=getmail(&scfg, user.number, /* sent: */FALSE)) > startup->max_msgs_waiting) {
					lprintf(LOG_NOTICE,"%04d !SMTP User #%u (%s) mailbox (%u msgs) exceeds the maximum (%u) msgs waiting"
						,socket, user.number, user.alias, waiting, startup->max_msgs_waiting);
					sockprintf(socket, "450 Mailbox full: %s", rcpt_to);
					stats.msgs_refused++;
					continue;
				}
			}
			else if(cmd==SMTP_CMD_SEND) { /* Check if user online */
				for(i=0;i<scfg.sys_nodes;i++) {
					getnodedat(&scfg, i+1, &node, 0);
					if(node.status==NODE_INUSE && node.useron==user.number
						&& !(node.misc&NODE_POFF))
						break;
				}
				if(i>=scfg.sys_nodes) {
					lprintf(LOG_WARNING,"%04d !Attempt to send telegram to unavailable user #%u (%s)"
						,socket, user.number, user.alias);
					sockprintf(socket,"450 User unavailable");
					continue;
				}
			}
			if(cmd!=SMTP_CMD_MAIL)
				telegram=TRUE;

			fprintf(rcptlst,"[%u]\n",rcpt_count++);
			fprintf(rcptlst,"%s=%s\n",smb_hfieldtype(RECIPIENT),rcpt_addr);
			fprintf(rcptlst,"%s=%u\n",smb_hfieldtype(RECIPIENTEXT),user.number);

			/* Forward to Internet */
			tp=strrchr(user.netmail,'@');
			if(!telegram
				&& !routed
				&& !no_forward
				&& scfg.sys_misc&SM_FWDTONET 
				&& (user.misc&NETMAIL || forward)
				&& tp!=NULL && smb_netaddr_type(user.netmail)==NET_INTERNET 
				&& !strstr(tp,scfg.sys_inetaddr)) {
				lprintf(LOG_INFO,"%04d SMTP Forwarding to: %s"
					,socket, user.netmail);
				fprintf(rcptlst,"%s=%u\n",smb_hfieldtype(RECIPIENTNETTYPE),NET_INTERNET);
				fprintf(rcptlst,"%s=%s\n",smb_hfieldtype(RECIPIENTNETADDR),user.netmail);
				sockprintf(socket,"251 User not local; will forward to %s", user.netmail);
			} else { /* Local (no-forward) */
				if(routed) { /* QWKnet */
					fprintf(rcptlst,"%s=%u\n",smb_hfieldtype(RECIPIENTNETTYPE),NET_QWK);
					fprintf(rcptlst,"%s=%s\n",smb_hfieldtype(RECIPIENTNETADDR),user.alias);
				}						
				sockprintf(socket,ok_rsp);
			}
			state=SMTP_STATE_RCPT_TO;
			continue;
		}
		/* Message Data (header and body) */
		if(!strnicmp(buf,"DATA",4)) {
			if(state<SMTP_STATE_RCPT_TO) {
				lprintf(LOG_WARNING,"%04d !SMTP MISSING 'RCPT TO' command", socket);
				sockprintf(socket, badseq_rsp);
				continue;
			}
			if(msgtxt!=NULL) {
				fclose(msgtxt), msgtxt=NULL;
			}
			remove(msgtxt_fname);
			if((msgtxt=fopen(msgtxt_fname,"w+b"))==NULL) {
				lprintf(LOG_ERR,"%04d !SMTP ERROR %d opening %s"
					,socket, errno, msgtxt_fname);
				sockprintf(socket, insuf_stor);
				continue;
			}
			/* These vars are potentially over-written by parsing an RFC822 header */
			/* get sender_addr */
			p=strrchr(reverse_path,'<');
			if(p==NULL)	
				p=reverse_path;
			else 
				p++;
			SAFECOPY(sender_addr,p);
			truncstr(sender_addr,">");
			/* get sender */
			SAFECOPY(sender,sender_addr);
			if(truncstr(sender,"@")==NULL)
				sender[0]=0;

			sockprintf(socket, "354 send the mail data, end with <CRLF>.<CRLF>");
			if(telegram)
				state=SMTP_STATE_DATA_BODY;	/* No RFC headers in Telegrams */
			else
				state=SMTP_STATE_DATA_HEADER;
			lprintf(LOG_INFO,"%04d SMTP Receiving %s message from: %s to %s"
				,socket, telegram ? "telegram":"mail", reverse_path, rcpt_addr);
			hdr_lines=0;
			continue;
		}
		sockprintf(socket,"500 Syntax error");
		lprintf(LOG_WARNING,"%04d !SMTP UNSUPPORTED COMMAND: '%s'", socket, buf);
		if(++badcmds>9) {
			lprintf(LOG_WARNING,"%04d !TOO MANY INVALID COMMANDS (%u)",socket,badcmds);
			break;
		}
	}

	/* Free up resources here */
	smb_freemsgmem(&msg);

	if(msgtxt!=NULL)
		fclose(msgtxt);
	if(!(startup->options&MAIL_OPT_DEBUG_RX_BODY))
		remove(msgtxt_fname);
	if(rcptlst!=NULL)
		fclose(rcptlst);
	remove(rcptlst_fname);
	if(spy!=NULL)
		fclose(spy);
	js_cleanup(js_runtime, js_cx, &js_glob);

	status(STATUS_WFC);

	protected_uint32_adjust(&active_clients, -1);
	update_clients();
	client_off(socket);

	{
		int32_t remain = thread_down();
		lprintf(LOG_INFO,"%04d SMTP Session thread terminated (%u threads remain, %lu clients served)"
			,socket, remain, ++stats.smtp_served);
	}
	free(mailproc_to_match);

	/* Must be last */
	mail_close_socket(socket);
}

BOOL bounce(SOCKET sock, smb_t* smb, smbmsg_t* msg, char* err, BOOL immediate)
{
	char		str[128];
	char		attempts[64];
	int			i;
	ushort		agent=AGENT_SMTPSYSMSG;
	smbmsg_t	newmsg;

	msg->hdr.delivery_attempts++;
	lprintf(LOG_WARNING,"%04d !SEND Delivery attempt #%u FAILED (%s) for message #%lu from %s to %s"
		,sock
		,msg->hdr.delivery_attempts
		,err
		,msg->hdr.number
		,msg->from
		,msg->to_net.addr);

	if((i=smb_updatemsg(smb,msg))!=SMB_SUCCESS) {
		lprintf(LOG_ERR,"%04d !SEND BOUNCE ERROR %d (%s) incrementing delivery attempt counter"
			,sock, i, smb->last_error);
		return(FALSE);
	}

	if(!immediate && msg->hdr.delivery_attempts < startup->max_delivery_attempts)
		return(TRUE);

	newmsg=*msg;
	/* Mark original message as deleted */
	msg->hdr.attr|=MSG_DELETE;

	i=smb_updatemsg(smb,msg);
	if(msg->hdr.auxattr&MSG_FILEATTACH)
		delfattach(&scfg,msg);
	if(i!=SMB_SUCCESS) {
		lprintf(LOG_ERR,"%04d !SEND BOUNCE ERROR %d (%s) deleting message"
			,sock, i, smb->last_error);
		return(FALSE);
	}

	if(msg->from_agent==AGENT_SMTPSYSMSG	/* don't bounce 'bounce messages' */
		|| (msg->hdr.attr&MSG_NOREPLY)
		|| (msg->idx.from==0 && msg->from_net.type==NET_NONE)
		|| (msg->reverse_path!=NULL && *msg->reverse_path==0)) {
		lprintf(LOG_WARNING,"%04d !SEND Deleted undeliverable message from %s", sock, msg->from);
		return(TRUE);
	}
	
	lprintf(LOG_WARNING,"%04d !SEND Bouncing message back to %s", sock, msg->from);

	newmsg.hfield=NULL;
	newmsg.hfield_dat=NULL;
	newmsg.total_hfields=0;
	newmsg.hdr.delivery_attempts=0;

	SAFEPRINTF(str,"Delivery failure: %s",newmsg.subj);
	smb_hfield_str(&newmsg, SUBJECT, str);
	smb_hfield_str(&newmsg, RECIPIENT, newmsg.from);
	if(msg->from_agent==AGENT_PERSON) {

		if(newmsg.from_ext!=NULL) { /* Back to sender */
			smb_hfield_str(&newmsg, RECIPIENTEXT, newmsg.from_ext);
			newmsg.from_ext=NULL;	/* Clear the sender extension */
		}

		if((newmsg.from_net.type==NET_QWK || newmsg.from_net.type==NET_INTERNET)
			&& newmsg.reverse_path!=NULL) {
			smb_hfield(&newmsg, RECIPIENTNETTYPE, sizeof(newmsg.from_net.type), &newmsg.from_net.type);
			smb_hfield_str(&newmsg, RECIPIENTNETADDR, newmsg.reverse_path);
		}
	} else {
		smb_hfield(&newmsg, RECIPIENTAGENT, sizeof(msg->from_agent), &msg->from_agent);
	}
	newmsg.hdr.attr|=MSG_NOREPLY;
	strcpy(str,"Mail Delivery Subsystem");
	smb_hfield_str(&newmsg, SENDER, str);
	smb_hfield(&newmsg, SENDERAGENT, sizeof(agent), &agent);
	
	/* Put error message in subject for now */
	if(msg->hdr.delivery_attempts>1)
		SAFEPRINTF(attempts,"after %u attempts", msg->hdr.delivery_attempts);
	else
		attempts[0]=0;
	SAFEPRINTF2(str,"%s reporting delivery failure of message %s"
		,startup->host_name, attempts);
	smb_hfield_str(&newmsg, SMB_COMMENT, str);
	SAFEPRINTF2(str,"from %s to %s\r\n"
		,msg->reverse_path==NULL ? msg->from : msg->reverse_path
		,(char*)msg->to_net.addr);
	smb_hfield_str(&newmsg, SMB_COMMENT, str);
	strcpy(str,"Reason:");
	smb_hfield_str(&newmsg, SMB_COMMENT, str);
	smb_hfield_str(&newmsg, SMB_COMMENT, err);
	smb_hfield_str(&newmsg, SMB_COMMENT, "\r\nOriginal message text follows:\r\n");

	if((i=smb_addmsghdr(smb,&newmsg,SMB_SELFPACK))!=SMB_SUCCESS)
		lprintf(LOG_ERR,"%04d !BOUNCE ERROR %d (%s) adding message header"
			,sock,i,smb->last_error);
	else {
		lprintf(LOG_WARNING,"%04d !SEND Delivery failure notification (message #%ld) created for %s"
			,sock, newmsg.hdr.number, newmsg.from);
		if((i=smb_incmsg_dfields(smb,&newmsg,1))!=SMB_SUCCESS)
			lprintf(LOG_ERR,"%04d !SEND BOUNCE ERROR %d (%s) incrementing data allocation units"
				,sock, i,smb->last_error);
	}

	newmsg.dfield=NULL;				/* Don't double-free the data fields */
	newmsg.hdr.total_dfields=0;
	smb_freemsgmem(&newmsg);

	return(TRUE);
}

static int remove_msg_intransit(smb_t* smb, smbmsg_t* msg)
{
	int i;

	if((i=smb_lockmsghdr(smb,msg))!=SMB_SUCCESS) {
		lprintf(LOG_WARNING,"0000 !SEND ERROR %d (%s) locking message header #%lu"
			,i, smb->last_error, msg->idx.number);
		return(i);
	}
	msg->hdr.netattr&=~MSG_INTRANSIT;
	i=smb_putmsghdr(smb,msg);
	smb_unlockmsghdr(smb,msg);
	
	if(i!=0)
		lprintf(LOG_ERR,"0000 !SEND ERROR %d (%s) writing message header #%lu"
			,i, smb->last_error, msg->idx.number);

	return(i);
}

void get_dns_server(char* dns_server, size_t len)
{
	str_list_t	list;
	size_t		count;

	sprintf(dns_server,"%.*s",len,startup->dns_server);
	if(!isalnum(dns_server[0])) {
		if((list=getNameServerList())!=NULL) {
			if((count=strListCount(list))>0) {
				sprintf(dns_server,"%.*s",len,list[xp_random(count)]);
				lprintf(LOG_DEBUG,"0000 SEND using auto-detected DNS server address: %s"
					,dns_server);
			}
			freeNameServerList(list);
		}
	}
}

#ifdef __BORLANDC__
#pragma argsused
#endif
static void sendmail_thread(void* arg)
{
	int			i,j;
	char		to[128];
	char		mx[128];
	char		mx2[128];
	char		err[1024];
	char		buf[512];
	char		str[128];
	char		resp[512];
	char		toaddr[256];
	char		fromext[128];
	char		fromaddr[256];
	char		challenge[256];
	char		secret[64];
	char		md5_data[384];
	uchar		digest[MD5_DIGEST_SIZE];
	char		numeric_ip[16];
	char		domain_list[MAX_PATH+1];
	char		dns_server[16];
	char*		server;
	char*		msgtxt=NULL;
	char*		p;
	char*		tp;
	ushort		port;
	ulong		last_msg=0;
	ulong		ip_addr;
	ulong		dns;
	ulong		lines;
	ulong		bytes;
	BOOL		success;
	BOOL		first_cycle=TRUE;
	SOCKET		sock=INVALID_SOCKET;
	SOCKADDR_IN	addr;
	SOCKADDR_IN	server_addr;
	time_t		last_scan=0;
	smb_t		smb;
	smbmsg_t	msg;
	mail_t*		mail;
	uint32_t	msgs;
	uint32_t	u;
	size_t		len;
	BOOL		sending_locally=FALSE;
	link_list_t	failed_server_list;

	SetThreadName("SendMail");
	thread_up(TRUE /* setuid */);

	sendmail_running=TRUE;
	terminate_sendmail=FALSE;

	lprintf(LOG_INFO,"0000 SendMail thread started");

	memset(&msg,0,sizeof(msg));
	memset(&smb,0,sizeof(smb));

	listInit(&failed_server_list, /* flags: */0);

	while(server_socket!=INVALID_SOCKET && !terminate_sendmail) {

		if(startup->options&MAIL_OPT_NO_SENDMAIL) {
			sem_trywait_block(&sendmail_wakeup_sem,1000);
			continue;
		}

		if(active_sendmail!=0)
			active_sendmail=0, update_clients();

		listFreeNodes(&failed_server_list);

		smb_close(&smb);

		if(sock!=INVALID_SOCKET) {
			mail_close_socket(sock);
			sock=INVALID_SOCKET;
		}

		if(msgtxt!=NULL) {
			smb_freemsgtxt(msgtxt);
			msgtxt=NULL;
		}

		smb_freemsgmem(&msg);

		/* Don't delay on first loop */
		if(first_cycle)
			first_cycle=FALSE;
		else
			sem_trywait_block(&sendmail_wakeup_sem,startup->sem_chk_freq*1000);

		SAFEPRINTF(smb.file,"%smail",scfg.data_dir);
		smb.retry_time=scfg.smb_retry_time;
		smb.subnum=INVALID_SUB;
		if((i=smb_open(&smb))!=SMB_SUCCESS) 
			continue;
		if((i=smb_locksmbhdr(&smb))!=SMB_SUCCESS)
			continue;
		i=smb_getstatus(&smb);
		smb_unlocksmbhdr(&smb);
		if(i!=0)
			continue;
		if(smb.status.last_msg==last_msg && time(NULL)-last_scan<startup->rescan_frequency)
			continue;
		lprintf(LOG_DEBUG, "0000 SEND last_msg=%u, smb.status.last_msg=%u, elapsed=%u"
			,last_msg, smb.status.last_msg, time(NULL)-last_scan);
		last_msg=smb.status.last_msg;
		last_scan=time(NULL);
		mail=loadmail(&smb,&msgs,/* to network */0,MAIL_YOUR,0);
		for(u=0; u<msgs; u++) {
			if(active_sendmail!=0)
				active_sendmail=0, update_clients();

			if(server_socket==INVALID_SOCKET || terminate_sendmail)	/* server stopped */
				break;

			if(sock!=INVALID_SOCKET) {
				mail_close_socket(sock);
				sock=INVALID_SOCKET;
			}

			if(msgtxt!=NULL) {
				smb_freemsgtxt(msgtxt);
				msgtxt=NULL;
			}

			smb_freemsgmem(&msg);

			msg.hdr.number=mail[u].number;
			if((i=smb_getmsgidx(&smb,&msg))!=SMB_SUCCESS) {
				lprintf(LOG_ERR,"0000 !SEND ERROR %d (%s) getting message index #%lu"
					,i, smb.last_error, mail[u].number);
				break;
			}
			if((i=smb_lockmsghdr(&smb,&msg))!=SMB_SUCCESS) {
				lprintf(LOG_WARNING,"0000 !SEND ERROR %d (%s) locking message header #%lu"
					,i, smb.last_error, msg.idx.number);
				continue;
			}
			if((i=smb_getmsghdr(&smb,&msg))!=SMB_SUCCESS) {
				smb_unlockmsghdr(&smb,&msg);
				lprintf(LOG_ERR,"0000 !SEND ERROR %d (%s) reading message header #%lu"
					,i, smb.last_error, msg.idx.number);
				continue; 
			}
			if(msg.hdr.attr&MSG_DELETE || msg.to_net.type!=NET_INTERNET || msg.to_net.addr==NULL) {
				smb_unlockmsghdr(&smb,&msg);
				continue;
			}

			if(!(startup->options&MAIL_OPT_SEND_INTRANSIT) && msg.hdr.netattr&MSG_INTRANSIT) {
				smb_unlockmsghdr(&smb,&msg);
				lprintf(LOG_NOTICE,"0000 SEND Message #%lu from %s to %s - in transit"
					,msg.hdr.number, msg.from, msg.to_net.addr);
				continue;
			}
			msg.hdr.netattr|=MSG_INTRANSIT;	/* Prevent another sendmail thread from sending this msg */
			smb_putmsghdr(&smb,&msg);
			smb_unlockmsghdr(&smb,&msg);

			active_sendmail=1, update_clients();

			fromext[0]=0;
			if(msg.from_ext)
				SAFEPRINTF(fromext," #%s", msg.from_ext);
			if(msg.from_net.type==NET_INTERNET && msg.reverse_path!=NULL)
				SAFECOPY(fromaddr,msg.reverse_path);
			else 
				usermailaddr(&scfg,fromaddr,msg.from);
			truncstr(fromaddr," ");

			lprintf(LOG_INFO,"0000 SEND Message #%lu (%u of %u) from %s%s %s to %s [%s]"
				,msg.hdr.number, u+1, msgs, msg.from, fromext, fromaddr
				,msg.to, msg.to_net.addr);
			SAFEPRINTF2(str,"Sending (%u of %u)", u+1, msgs);
			status(str);
#ifdef _WIN32
			if(startup->outbound_sound[0] && !(startup->options&MAIL_OPT_MUTE)) 
				PlaySound(startup->outbound_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif

			lprintf(LOG_DEBUG,"0000 SEND getting message text");
			if((msgtxt=smb_getmsgtxt(&smb,&msg,GETMSGTXT_ALL))==NULL) {
				remove_msg_intransit(&smb,&msg);
				lprintf(LOG_ERR,"0000 !SEND ERROR (%s) retrieving message text",smb.last_error);
				continue;
			}

			remove_ctrl_a(msgtxt, msgtxt);

			port=0;
			mx2[0]=0;

			sending_locally=FALSE;
			/* Check if this is a local email ToDo */
			SAFECOPY(to,(char*)msg.to_net.addr);
			truncstr(to,"> ");

			p=strrchr(to,'@');
			if(p==NULL) {
				remove_msg_intransit(&smb,&msg);
				lprintf(LOG_WARNING,"0000 !SEND INVALID destination address: %s", to);
				SAFEPRINTF(err,"Invalid destination address: %s", to);
				bounce(0, &smb,&msg,err, /* immediate: */TRUE);
				continue;
			}
			p++;
			SAFEPRINTF(domain_list,"%sdomains.cfg",scfg.ctrl_dir);
			if(stricmp(p,scfg.sys_inetaddr)==0
					|| stricmp(p,startup->host_name)==0
					|| findstr(p,domain_list)) {
				/* This is a local message... no need to send to remote */
				port = startup->smtp_port;
				if(startup->interface_addr==0)
					server="127.0.0.1";
				else {
					SAFEPRINTF4(numeric_ip, "%u.%u.%u.%u"
							, startup->interface_addr >> 24
							, (startup->interface_addr >> 16) & 0xff
							, (startup->interface_addr >> 8) & 0xff
							, startup->interface_addr & 0xff);
					server = numeric_ip;
				}
				sending_locally=TRUE;
			}
			else {
				if(startup->options&MAIL_OPT_RELAY_TX) { 
					server=startup->relay_server;
					port=startup->relay_port;
				} else {
					server=p;
					tp=strrchr(p,':');	/* non-standard SMTP port */
					if(tp!=NULL) {
						*tp=0;
						port=atoi(tp+1);
					}
					if(port==0) {	/* No port specified, use MX look-up */
						get_dns_server(dns_server,sizeof(dns_server));
						if((dns=resolve_ip(dns_server))==INADDR_NONE) {
							remove_msg_intransit(&smb,&msg);
							lprintf(LOG_WARNING,"0000 !SEND INVALID DNS server address: %s"
								,dns_server);
							continue;
						}
						lprintf(LOG_DEBUG,"0000 SEND getting MX records for %s from %s",p,dns_server);
						if((i=dns_getmx(p, mx, mx2, INADDR_ANY, dns
							,startup->options&MAIL_OPT_USE_TCP_DNS ? TRUE : FALSE
							,TIMEOUT_THREAD_WAIT/2))!=0) {
							remove_msg_intransit(&smb,&msg);
							lprintf(LOG_WARNING,"0000 !SEND ERROR %d obtaining MX records for %s from %s"
								,i,p,dns_server);
							SAFEPRINTF2(err,"Error %d obtaining MX record for %s",i,p);
							bounce(0, &smb,&msg,err, /* immediate: */FALSE);
							continue;
						}
						server=mx;
					}
				}
			}
			if(!port)
				port=IPPORT_SMTP;

			if((sock=mail_open_socket(SOCK_STREAM,"smtp|sendmail"))==INVALID_SOCKET) {
				remove_msg_intransit(&smb,&msg);
				lprintf(LOG_ERR,"0000 !SEND ERROR %d opening socket", ERROR_VALUE);
				continue;
			}

			if(startup->connect_timeout) {	/* Use non-blocking socket */
				long nbio=1;
				if((i=ioctlsocket(sock, FIONBIO, &nbio))!=0) {
					remove_msg_intransit(&smb,&msg);
					lprintf(LOG_ERR,"%04d !SEND ERROR %d (%d) disabling blocking on socket"
						,sock, i, ERROR_VALUE);
					continue;
				}
			}

			memset(&addr,0,sizeof(addr));
			addr.sin_addr.s_addr = htonl(startup->interface_addr);
			addr.sin_family = AF_INET;

			/* Not needed.  Port is zero
			if(startup->seteuid!=NULL)
				startup->seteuid(FALSE); */
			i=bind(sock,(struct sockaddr *)&addr, sizeof(addr));
			/* Not needed.  Port is zero
			if(startup->seteuid!=NULL)
				startup->seteuid(TRUE); */
			if(i!=0) {
				remove_msg_intransit(&smb,&msg);
				lprintf(LOG_ERR,"%04d !SEND ERROR %d (%d) binding socket", sock, i, ERROR_VALUE);
				continue;
			}

			strcpy(err,"UNKNOWN ERROR");
			success=FALSE;
			for(j=0;j<2 && !success;j++) {
				list_node_t*	node;

				if(j) {
					if(startup->options&MAIL_OPT_RELAY_TX || !mx2[0])
						break;
					lprintf(LOG_DEBUG,"%04d SEND reverting to second MX: %s", sock, mx2);
					server=mx2;	/* Give second mx record a try */
				}
				
				lprintf(LOG_DEBUG,"%04d SEND resolving SMTP hostname: %s", sock, server);
				ip_addr=resolve_ip(server);
				if(ip_addr==INADDR_NONE) {
					SAFEPRINTF(err,"Failed to resolve SMTP hostname: %s",server);
					lprintf(LOG_WARNING,"%04d !SEND failure resolving hostname: %s", sock, server);
					continue;
				}

				memset(&server_addr,0,sizeof(server_addr));
				server_addr.sin_addr.s_addr = ip_addr;
				server_addr.sin_family = AF_INET;
				server_addr.sin_port = htons(port);

				if((node=listFindNode(&failed_server_list,&server_addr,sizeof(server_addr))) != NULL) {
					lprintf(LOG_INFO,"%04d SEND skipping failed SMTP server: Error %d connecting to port %u on %s [%s]"
					,sock
					,node->tag
					,ntohs(server_addr.sin_port)
					,server,inet_ntoa(server_addr.sin_addr));
					SAFEPRINTF2(err,"Error %d connecting to SMTP server: %s"
						,node->tag, server);
					continue;
				}

				if((server==mx || server==mx2) 
					&& ((ip_addr&0xff)==127 || ip_addr==0)) {
					SAFEPRINTF2(err,"Bad IP address (%s) for MX server: %s"
						,inet_ntoa(server_addr.sin_addr),server);
					continue;
				}
				
				lprintf(LOG_INFO,"%04d SEND connecting to port %u on %s [%s]"
					,sock
					,ntohs(server_addr.sin_port)
					,server,inet_ntoa(server_addr.sin_addr));
				if((i=nonblocking_connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr), startup->connect_timeout))!=0) {
					lprintf(LOG_WARNING,"%04d !SEND ERROR %d connecting to SMTP server: %s"
						,sock
						,i, server);
					SAFEPRINTF2(err,"Error %d connecting to SMTP server: %s"
						,i, server);
					listAddNodeData(&failed_server_list,&server_addr,sizeof(server_addr),i,NULL);
					continue;
				}
				success=TRUE;
			}
			if(!success) {	/* Failed to send, so bounce */
				remove_msg_intransit(&smb,&msg);
				bounce(sock, &smb,&msg,err,/* immediate: */FALSE);	
				continue;
			}

			lprintf(LOG_DEBUG,"%04d SEND connected to %s",sock,server);

			/* HELO */
			if(!sockgetrsp(sock,"220",buf,sizeof(buf))) {
				remove_msg_intransit(&smb,&msg);
				SAFEPRINTF3(err,badrsp_err,server,buf,"220");
				bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
				continue;
			}
			if(startup->options&MAIL_OPT_RELAY_TX 
				&& (startup->options&MAIL_OPT_RELAY_AUTH_MASK)!=0)	/* Requires ESMTP */
				sockprintf(sock,"EHLO %s",startup->host_name);
			else
				sockprintf(sock,"HELO %s",startup->host_name);
			if(!sockgetrsp(sock,"250", buf, sizeof(buf))) {
				remove_msg_intransit(&smb,&msg);
				SAFEPRINTF3(err,badrsp_err,server,buf,"250");
				bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
				continue;
			}

			/* AUTH */
			if(startup->options&MAIL_OPT_RELAY_TX 
				&& (startup->options&MAIL_OPT_RELAY_AUTH_MASK)!=0 && !sending_locally) {

				if((startup->options&MAIL_OPT_RELAY_AUTH_MASK)==MAIL_OPT_RELAY_AUTH_PLAIN) {
					/* Build the buffer: <username>\0<user-id>\0<password */
					len=safe_snprintf(buf,sizeof(buf),"%s%c%s%c%s"
						,startup->relay_user
						,0
						,startup->relay_user
						,0
						,startup->relay_pass);
					b64_encode(resp,sizeof(resp),buf,len);
					sockprintf(sock,"AUTH PLAIN %s",resp);
				} else {
					switch(startup->options&MAIL_OPT_RELAY_AUTH_MASK) {
						case MAIL_OPT_RELAY_AUTH_LOGIN:
							p="LOGIN";
							break;
						case MAIL_OPT_RELAY_AUTH_CRAM_MD5:
							p="CRAM-MD5";
							break;
						default:
							p="<unknown>";
							break;
					}
					sockprintf(sock,"AUTH %s",p);
					if(!sockgetrsp(sock,"334",buf,sizeof(buf))) {
						SAFEPRINTF3(err,badrsp_err,server,buf,"334 Username/Challenge");
						bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
						continue;
					}
					switch(startup->options&MAIL_OPT_RELAY_AUTH_MASK) {
						case MAIL_OPT_RELAY_AUTH_LOGIN:
							b64_encode(p=resp,sizeof(resp),startup->relay_user,0);
							break;
						case MAIL_OPT_RELAY_AUTH_CRAM_MD5:
							p=buf;
							FIND_WHITESPACE(p);
							SKIP_WHITESPACE(p);
							b64_decode(challenge,sizeof(challenge),p,0);

							/* Calculate response */
							memset(secret,0,sizeof(secret));
							SAFECOPY(secret,startup->relay_pass);
							for(i=0;i<sizeof(secret);i++)
								md5_data[i]=secret[i]^0x36;	/* ipad */
							strcpy(md5_data+i,challenge);
							MD5_calc(digest,md5_data,sizeof(secret)+strlen(challenge));
							for(i=0;i<sizeof(secret);i++)
								md5_data[i]=secret[i]^0x5c;	/* opad */
							memcpy(md5_data+i,digest,sizeof(digest));
							MD5_calc(digest,md5_data,sizeof(secret)+sizeof(digest));
							
							safe_snprintf(buf,sizeof(buf),"%s %s",startup->relay_user,MD5_hex((BYTE*)str,digest));
							b64_encode(p=resp,sizeof(resp),buf,0);
							break;
						default:
							p="<unknown>";
							break;
					}
					sockprintf(sock,"%s",p);
					if((startup->options&MAIL_OPT_RELAY_AUTH_MASK)!=MAIL_OPT_RELAY_AUTH_CRAM_MD5) {
						if(!sockgetrsp(sock,"334",buf,sizeof(buf))) {
							SAFEPRINTF3(err,badrsp_err,server,buf,"334 Password");
							bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
							continue;
						}
						switch(startup->options&MAIL_OPT_RELAY_AUTH_MASK) {
							case MAIL_OPT_RELAY_AUTH_LOGIN:
								b64_encode(p=buf,sizeof(buf),startup->relay_pass,0);
								break;
							default:
								p="<unknown>";
								break;
						}
						sockprintf(sock,"%s",p);
					}
				}
				if(!sockgetrsp(sock,"235",buf,sizeof(buf))) {
					SAFEPRINTF3(err,badrsp_err,server,buf,"235");
					bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
					continue;
				}
			}

			/* MAIL */
			if(fromaddr[0]=='<')
				sockprintf(sock,"MAIL FROM: %s",fromaddr);
			else
				sockprintf(sock,"MAIL FROM: <%s>",fromaddr);
			if(!sockgetrsp(sock,"250", buf, sizeof(buf))) {
				remove_msg_intransit(&smb,&msg);
				SAFEPRINTF3(err,badrsp_err,server,buf,"250");
				bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
				continue;
			}
			/* RCPT */
			if(msg.forward_path!=NULL) {
				SAFECOPY(toaddr,msg.forward_path);
			} else {
				if((p=strrchr((char*)msg.to_net.addr,'<'))!=NULL)
					p++;
				else
					p=(char*)msg.to_net.addr;
				SAFECOPY(toaddr,p);
				truncstr(toaddr,"> ");
				if((p=strrchr(toaddr,'@'))!=NULL && (tp=strrchr(toaddr,':'))!=NULL
					&& tp > p)
					*tp=0;	/* Remove ":port" designation from envelope */
			}
			sockprintf(sock,"RCPT TO: <%s>", toaddr);
			if(!sockgetrsp(sock,"25", buf, sizeof(buf))) {
				remove_msg_intransit(&smb,&msg);
				SAFEPRINTF3(err,badrsp_err,server,buf,"25*");
				bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
				continue;
			}
			/* DATA */
			sockprintf(sock,"DATA");
			if(!sockgetrsp(sock,"354", buf, sizeof(buf))) {
				remove_msg_intransit(&smb,&msg);
				SAFEPRINTF3(err,badrsp_err,server,buf,"354");
				bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
				continue;
			}
			bytes=strlen(msgtxt);
			lprintf(LOG_DEBUG,"%04d SEND sending message text (%u bytes) begin"
				,sock, bytes);
			lines=sockmsgtxt(sock,&msg,msgtxt,-1);
			lprintf(LOG_DEBUG,"%04d SEND send of message text (%u bytes, %u lines) complete, waiting for acknowledgement (250)"
				,sock, bytes, lines);
			if(!sockgetrsp(sock,"250", buf, sizeof(buf))) {
				/* Wait doublely-long for the acknowledgement */
				if(buf[0] || !sockgetrsp(sock,"250", buf, sizeof(buf))) {
					remove_msg_intransit(&smb,&msg);
					SAFEPRINTF3(err,badrsp_err,server,buf,"250");
					bounce(sock, &smb,&msg,err,/* immediate: */buf[0]=='5');
					continue;
				}
			}
			lprintf(LOG_INFO,"%04d SEND message transfer complete (%u bytes, %lu lines)", sock, bytes, lines);

			/* Now lets mark this message for deletion without corrupting the index */
			msg.hdr.attr|=MSG_DELETE;
			msg.hdr.netattr&=~MSG_INTRANSIT;
			if((i=smb_updatemsg(&smb,&msg))!=SMB_SUCCESS)
				lprintf(LOG_ERR,"%04d !SEND ERROR %d (%s) deleting message #%lu"
					,sock, i, smb.last_error, msg.hdr.number);
			if(msg.hdr.auxattr&MSG_FILEATTACH)
				delfattach(&scfg,&msg);

			if(msg.from_agent==AGENT_PERSON && !(startup->options&MAIL_OPT_NO_AUTO_EXEMPT))
				exempt_email_addr("SEND Auto-exempting",msg.from,fromext,fromaddr,toaddr);

			/* QUIT */
			sockprintf(sock,"QUIT");
			sockgetrsp(sock,"221", buf, sizeof(buf));
			mail_close_socket(sock);
			sock=INVALID_SOCKET;
		}				
		status(STATUS_WFC);
		/* Free up resources here */
		if(mail!=NULL)
			freemail(mail);
	}
	if(sock!=INVALID_SOCKET)
		mail_close_socket(sock);

	listFree(&failed_server_list);

	smb_freemsgtxt(msgtxt);
	smb_freemsgmem(&msg);
	smb_close(&smb);

	if(active_sendmail!=0)
		active_sendmail=0, update_clients();

	{
		int32_t remain = thread_down();
		lprintf(LOG_DEBUG,"0000 SendMail thread terminated (%u threads remain)", remain);
	}

	sendmail_running=FALSE;
}

void DLLCALL mail_terminate(void)
{
  	lprintf(LOG_INFO,"%04d Mail Server terminate",server_socket);
	terminate_server=TRUE;
}

static void cleanup(int code)
{
	int					i;

	free_cfg(&scfg);

	semfile_list_free(&recycle_semfiles);
	semfile_list_free(&shutdown_semfiles);

	if(mailproc_list!=NULL) {
		for(i=0;i<mailproc_count;i++) {
			if(mailproc_list[i].ar!=NULL && mailproc_list[i].ar!=nular)
				free(mailproc_list[i].ar);
			strListFree(&mailproc_list[i].to);
			strListFree(&mailproc_list[i].from);
		}
		FREE_AND_NULL(mailproc_list);
	}

	if(server_socket!=INVALID_SOCKET) {
		mail_close_socket(server_socket);
		server_socket=INVALID_SOCKET;
	}

	if(submission_socket!=INVALID_SOCKET) {
		mail_close_socket(submission_socket);
		submission_socket=INVALID_SOCKET;
	}

	if(pop3_socket!=INVALID_SOCKET) {
		mail_close_socket(pop3_socket);
		pop3_socket=INVALID_SOCKET;
	}

	if(thread_count.value > 1) {
		lprintf(LOG_DEBUG,"#### Waiting for %d child threads to terminate", thread_count.value-1);
		while(thread_count.value > 1) {
			mswait(100);
		}
	}

	if(active_clients.value)
		lprintf(LOG_WARNING,"#### !Mail Server terminating with %ld active clients", active_clients.value);
	else
		protected_uint32_destroy(active_clients);

	update_clients();

#ifdef _WINSOCKAPI_	
	if(WSAInitialized && WSACleanup()!=0) 
		lprintf(LOG_ERR,"0000 !WSACleanup ERROR %d",ERROR_VALUE);
#endif

	thread_down();
	status("Down");
	if(terminate_server || code) {
		char str[1024];
		sprintf(str,"%lu connections served", stats.connections_served);
		if(stats.connections_refused)
			sprintf(str+strlen(str),", %lu refused", stats.connections_refused);
		if(stats.connections_ignored)
			sprintf(str+strlen(str),", %lu ignored", stats.connections_refused);
		if(stats.sessions_refused)
			sprintf(str+strlen(str),", %lu sessions refused", stats.sessions_refused);
		sprintf(str+strlen(str),", %lu messages received", stats.msgs_received);
		if(stats.msgs_refused)
			sprintf(str+strlen(str),", %lu refused", stats.msgs_refused);
		if(stats.msgs_ignored)
			sprintf(str+strlen(str),", %lu ignored", stats.msgs_ignored);
		if(stats.errors)
			sprintf(str+strlen(str),", %lu errors", stats.errors);
		if(stats.crit_errors)
			sprintf(str+strlen(str),", %lu critcal", stats.crit_errors);

		lprintf(LOG_INFO,"#### Mail Server thread terminated (%s)",str);
	}
	if(startup!=NULL && startup->terminated!=NULL)
		startup->terminated(startup->cbdata,code);
}

const char* DLLCALL mail_ver(void)
{
	static char ver[256];
	char compiler[32];

	DESCRIBE_COMPILER(compiler);

	sscanf("$Revision: 1.563 $", "%*s %s", revision);

	sprintf(ver,"%s %s%s  SMBLIB %s  "
		"Compiled %s %s with %s"
		,server_name
		,revision
#ifdef _DEBUG
		," Debug"
#else
		,""
#endif
		,smb_lib_ver()
		,__DATE__, __TIME__, compiler
		);

	return(ver);
}

void DLLCALL mail_server(void* arg)
{
	char*			p;
	char			path[MAX_PATH+1];
	char			mailproc_ini[MAX_PATH+1];
	char			str[256];
	char			error[256];
	char			compiler[32];
	SOCKADDR_IN		server_addr;
	SOCKADDR_IN		client_addr;
	socklen_t		client_addr_len;
	SOCKET			client_socket;
	int				i;
	int				result;
	ulong			l;
	time_t			t;
	time_t			start;
	time_t			initialized=0;
	fd_set			socket_set;
	SOCKET			high_socket_set;
	pop3_t*			pop3;
	smtp_t*			smtp;
	struct timeval	tv;
	FILE*			fp;
	str_list_t		sec_list;

	mail_ver();

	startup=(mail_startup_t*)arg;

#ifdef _THREAD_SUID_BROKEN
	if(thread_suid_broken)
		startup->seteuid(TRUE);
#endif

    if(startup==NULL) {
    	sbbs_beep(100,500);
    	fprintf(stderr, "No startup structure passed!\n");
    	return;
    }

	if(startup->size!=sizeof(mail_startup_t)) {	/* verify size */
		sbbs_beep(100,500);
		sbbs_beep(300,500);
		sbbs_beep(100,500);
		fprintf(stderr, "Invalid startup structure!\n");
		return;
	}

	/* Setup intelligent defaults */
	if(startup->relay_port==0)				startup->relay_port=IPPORT_SMTP;
	if(startup->submission_port==0)			startup->submission_port=IPPORT_SUBMISSION;
	if(startup->smtp_port==0)				startup->smtp_port=IPPORT_SMTP;
	if(startup->pop3_port==0)				startup->pop3_port=IPPORT_POP3;
	if(startup->rescan_frequency==0)		startup->rescan_frequency=3600;	/* 60 minutes */
	if(startup->max_delivery_attempts==0)	startup->max_delivery_attempts=50;
	if(startup->max_inactivity==0) 			startup->max_inactivity=120; /* seconds */
	if(startup->sem_chk_freq==0)			startup->sem_chk_freq=2;

#ifdef JAVASCRIPT
	if(startup->js.max_bytes==0)			startup->js.max_bytes=JAVASCRIPT_MAX_BYTES;
	if(startup->js.cx_stack==0)				startup->js.cx_stack=JAVASCRIPT_CONTEXT_STACK;
#endif

	ZERO_VAR(js_server_props);
	SAFEPRINTF2(js_server_props.version,"%s %s",server_name,revision);
	js_server_props.version_detail=mail_ver();
	js_server_props.clients=&active_clients.value;
	js_server_props.options=&startup->options;
	js_server_props.interface_addr=&startup->interface_addr;

	uptime=0;
	memset(&stats,0,sizeof(stats));
	startup->recycle_now=FALSE;
	startup->shutdown_now=FALSE;
	terminate_server=FALSE;

	SetThreadName("Mail Server");
	protected_uint32_init(&thread_count, 0);

	do {

		thread_up(FALSE /* setuid */);

		status("Initializing");

		memset(&scfg, 0, sizeof(scfg));

		lprintf(LOG_INFO,"%s Revision %s%s"
			,server_name
			,revision
#ifdef _DEBUG
			," Debug"
#else
			,""
#endif
			);

		DESCRIBE_COMPILER(compiler);

		lprintf(LOG_INFO,"Compiled %s %s with %s", __DATE__, __TIME__, compiler);

		lprintf(LOG_DEBUG,"SMBLIB %s (format %x.%02x)",smb_lib_ver(),smb_ver()>>8,smb_ver()&0xff);

		sbbs_srand();

		if(!winsock_startup()) {
			cleanup(1);
			return;
		}

		t=time(NULL);
		lprintf(LOG_INFO,"Initializing on %.24s with options: %lx"
			,ctime_r(&t,str),startup->options);

		if(chdir(startup->ctrl_dir)!=0)
			lprintf(LOG_ERR,"!ERROR %d changing directory to: %s", errno, startup->ctrl_dir);

		/* Initial configuration and load from CNF files */
		SAFECOPY(scfg.ctrl_dir,startup->ctrl_dir);
		lprintf(LOG_INFO,"Loading configuration files from %s", scfg.ctrl_dir);
		scfg.size=sizeof(scfg);
		SAFECOPY(error,UNKNOWN_LOAD_ERROR);
		if(!load_cfg(&scfg, NULL, TRUE, error)) {
			lprintf(LOG_CRIT,"!ERROR %s",error);
			lprintf(LOG_CRIT,"!Failed to load configuration files");
			cleanup(1);
			return;
		}

		if(startup->temp_dir[0])
			SAFECOPY(scfg.temp_dir,startup->temp_dir);
		else
			SAFECOPY(scfg.temp_dir,"../temp");
	   	prep_dir(scfg.ctrl_dir, scfg.temp_dir, sizeof(scfg.temp_dir));
		MKDIR(scfg.temp_dir);
		lprintf(LOG_DEBUG,"Temporary file directory: %s", scfg.temp_dir);
		if(!isdir(scfg.temp_dir)) {
			lprintf(LOG_CRIT,"!Invalid temp directory: %s", scfg.temp_dir);
			cleanup(1);
			return;
		}

		/* Parse the mailproc[.host].ini */
		mailproc_list=NULL;
		mailproc_count=0;
		iniFileName(mailproc_ini,sizeof(mailproc_ini),scfg.ctrl_dir,"mailproc.ini");
		if((fp=iniOpenFile(mailproc_ini, /* create? */FALSE))!=NULL) {
			lprintf(LOG_DEBUG,"Reading %s",mailproc_ini);
			sec_list = iniReadSectionList(fp,/* prefix */NULL);
			if((mailproc_count=strListCount(sec_list))!=0
				&& (mailproc_list=malloc(mailproc_count*sizeof(struct mailproc)))!=NULL) {
				char buf[INI_MAX_VALUE_LEN+1];
				for(i=0;i<mailproc_count;i++) {
					memset(&mailproc_list[i],0,sizeof(struct mailproc));
					SAFECOPY(mailproc_list[i].name,sec_list[i]);
					SAFECOPY(mailproc_list[i].cmdline,
						iniReadString(fp,sec_list[i],"Command",sec_list[i],buf));
					SAFECOPY(mailproc_list[i].eval,
						iniReadString(fp,sec_list[i],"Eval","",buf));
					mailproc_list[i].to =
						iniReadStringList(fp,sec_list[i],"To",",",NULL);
					mailproc_list[i].from =
						iniReadStringList(fp,sec_list[i],"From",",",NULL);
					mailproc_list[i].passthru =
						iniReadBool(fp,sec_list[i],"PassThru",TRUE);
					mailproc_list[i].native =
						iniReadBool(fp,sec_list[i],"Native",FALSE);
					mailproc_list[i].disabled = 
						iniReadBool(fp,sec_list[i],"Disabled",FALSE);
					mailproc_list[i].ignore_on_error = 
						iniReadBool(fp,sec_list[i],"IgnoreOnError",FALSE);
					mailproc_list[i].process_spam =
						iniReadBool(fp,sec_list[i],"ProcessSPAM",TRUE);
					mailproc_list[i].process_dnsbl =
						iniReadBool(fp,sec_list[i],"ProcessDNSBL",TRUE);
					mailproc_list[i].ar = 
						arstr(NULL,iniReadString(fp,sec_list[i],"AccessRequirements","",buf),&scfg);
				}
			}
			iniFreeStringList(sec_list);
			iniCloseFile(fp);
		}

		if(startup->host_name[0]==0)
			SAFECOPY(startup->host_name,scfg.sys_inetaddr);

		if((t=checktime())!=0) {   /* Check binary time */
			lprintf(LOG_ERR,"!TIME PROBLEM (%ld)",t);
		}

		if(uptime==0)
			uptime=time(NULL);	/* this must be done *after* setting the timezone */

		if(startup->max_clients==0) {
			startup->max_clients=scfg.sys_nodes;
			if(startup->max_clients<10)
				startup->max_clients=10;
		}

		lprintf(LOG_DEBUG,"Maximum clients: %u",startup->max_clients);

		lprintf(LOG_DEBUG,"Maximum inactivity: %u seconds",startup->max_inactivity);

		protected_uint32_init(&active_clients, 0);
		update_clients();

		/* open a socket and wait for a client */

		server_socket = mail_open_socket(SOCK_STREAM,"smtp");

		if(server_socket == INVALID_SOCKET) {
			lprintf(LOG_CRIT,"!ERROR %d opening socket", ERROR_VALUE);
			cleanup(1);
			return;
		}

		lprintf(LOG_DEBUG,"%04d SMTP socket opened",server_socket);

		/*****************************/
		/* Listen for incoming calls */
		/*****************************/
		memset(&server_addr, 0, sizeof(server_addr));

		server_addr.sin_addr.s_addr = htonl(startup->interface_addr);
		server_addr.sin_family = AF_INET;
		server_addr.sin_port   = htons(startup->smtp_port);

		if(startup->smtp_port < IPPORT_RESERVED) {
			if(startup->seteuid!=NULL)
				startup->seteuid(FALSE);
		}
		result = retry_bind(server_socket,(struct sockaddr *)&server_addr,sizeof(server_addr)
			,startup->bind_retry_count,startup->bind_retry_delay,"SMTP Server",lprintf);
		if(startup->smtp_port < IPPORT_RESERVED) {
			if(startup->seteuid!=NULL)
				startup->seteuid(TRUE);
		}
		if(result != 0) {
			lprintf(LOG_CRIT,"%04d %s",server_socket, BIND_FAILURE_HELP);
			cleanup(1);
			return;
		}
		result = listen(server_socket, 1);

		if(result != 0) {
			lprintf(LOG_CRIT,"%04d !ERROR %d (%d) listening on SMTP socket"
				,server_socket, result, ERROR_VALUE);
			cleanup(1);
			return;
		}
		lprintf(LOG_INFO,"%04d SMTP Server listening on port %u"
			,server_socket, startup->smtp_port);

		if(startup->options&MAIL_OPT_USE_SUBMISSION_PORT) {

			submission_socket = mail_open_socket(SOCK_STREAM,"submission");

			if(submission_socket == INVALID_SOCKET) {
				lprintf(LOG_CRIT,"!ERROR %d opening socket", ERROR_VALUE);
				cleanup(1);
				return;
			}

			lprintf(LOG_DEBUG,"%04d SUBMISSION socket opened",submission_socket);

			/*****************************/
			/* Listen for incoming calls */
			/*****************************/
			memset(&server_addr, 0, sizeof(server_addr));

			server_addr.sin_addr.s_addr = htonl(startup->interface_addr);
			server_addr.sin_family = AF_INET;
			server_addr.sin_port   = htons(startup->submission_port);

			if(startup->submission_port < IPPORT_RESERVED) {
				if(startup->seteuid!=NULL)
					startup->seteuid(FALSE);
			}
			result = retry_bind(submission_socket,(struct sockaddr *)&server_addr,sizeof(server_addr)
				,startup->bind_retry_count,startup->bind_retry_delay,"SMTP Submission Agent",lprintf);
			if(startup->submission_port < IPPORT_RESERVED) {
				if(startup->seteuid!=NULL)
					startup->seteuid(TRUE);
			}
			if(result != 0) {
				lprintf(LOG_CRIT,"%04d %s",submission_socket, BIND_FAILURE_HELP);
				cleanup(1);
				return;
			}

			result = listen(submission_socket, 1);

			if(result != 0) {
				lprintf(LOG_CRIT,"%04d !ERROR %d (%d) listening on SUBMISSION socket"
					,submission_socket, result, ERROR_VALUE);
				cleanup(1);
				return;
			}

			lprintf(LOG_INFO,"%04d SUBMISSION Server listening on port %u"
				,submission_socket, startup->submission_port);
		}

		if(startup->options&MAIL_OPT_ALLOW_POP3) {

			/* open a socket and wait for a client */

			pop3_socket = mail_open_socket(SOCK_STREAM,"pop3");

			if(pop3_socket == INVALID_SOCKET) {
				lprintf(LOG_CRIT,"!ERROR %d opening POP3 socket", ERROR_VALUE);
				cleanup(1);
				return;
			}

			lprintf(LOG_DEBUG,"%04d POP3 socket opened",pop3_socket);

			/*****************************/
			/* Listen for incoming calls */
			/*****************************/
			memset(&server_addr, 0, sizeof(server_addr));

			server_addr.sin_addr.s_addr = htonl(startup->interface_addr);
			server_addr.sin_family = AF_INET;
			server_addr.sin_port   = htons(startup->pop3_port);

			if(startup->pop3_port < IPPORT_RESERVED) {
				if(startup->seteuid!=NULL)
					startup->seteuid(FALSE);
			}
			result = retry_bind(pop3_socket,(struct sockaddr *)&server_addr,sizeof(server_addr)
				,startup->bind_retry_count,startup->bind_retry_delay,"POP3 Server",lprintf);
			if(startup->pop3_port < IPPORT_RESERVED) {
				if(startup->seteuid!=NULL)
					startup->seteuid(FALSE);
			}
			if(result != 0) {
				lprintf(LOG_CRIT,"%04d %s",pop3_socket,BIND_FAILURE_HELP);
				cleanup(1);
				return;
			}

			result = listen(pop3_socket, 1);

			if(result != 0) {
				lprintf(LOG_CRIT,"%04d !ERROR %d (%d) listening on POP3 socket"
					,pop3_socket, result, ERROR_VALUE);
				cleanup(1);
				return;
			}

			lprintf(LOG_INFO,"%04d POP3 Server listening on port %u"
				,pop3_socket, startup->pop3_port);
		}

		sem_init(&sendmail_wakeup_sem,0,0);

		if(!(startup->options&MAIL_OPT_NO_SENDMAIL))
			_beginthread(sendmail_thread, 0, NULL);

		status(STATUS_WFC);

		/* Setup recycle/shutdown semaphore file lists */
		shutdown_semfiles=semfile_list_init(scfg.ctrl_dir,"shutdown","mail");
		recycle_semfiles=semfile_list_init(scfg.ctrl_dir,"recycle","mail");
		SAFEPRINTF(path,"%smailsrvr.rec",scfg.ctrl_dir);	/* legacy */
		semfile_list_add(&recycle_semfiles,path);
		semfile_list_add(&recycle_semfiles,mailproc_ini);
		if(!initialized) {
			semfile_list_check(&initialized,recycle_semfiles);
			semfile_list_check(&initialized,shutdown_semfiles);
		}

		/* signal caller that we've started up successfully */
		if(startup->started!=NULL)
    		startup->started(startup->cbdata);

		lprintf(LOG_INFO,"%04d Mail Server thread started",server_socket);

		while(server_socket!=INVALID_SOCKET && !terminate_server) {

			if(active_clients.value==0) {
				if(!(startup->options&MAIL_OPT_NO_RECYCLE)) {
					if((p=semfile_list_check(&initialized,recycle_semfiles))!=NULL) {
						lprintf(LOG_INFO,"%04d Recycle semaphore file (%s) detected"
							,server_socket,p);
						break;
					}
					if(startup->recycle_now==TRUE) {
						lprintf(LOG_NOTICE,"%04d Recycle semaphore signaled", server_socket);
						startup->recycle_now=FALSE;
						break;
					}
				}
				if(((p=semfile_list_check(&initialized,shutdown_semfiles))!=NULL
						&& lprintf(LOG_INFO,"%04d Shutdown semaphore file (%s) detected"
						,server_socket,p))
					|| (startup->shutdown_now==TRUE
						&& lprintf(LOG_INFO,"%04d Shutdown semaphore signaled",server_socket))) {
					startup->shutdown_now=FALSE;
					terminate_server=TRUE;
					break;
				}
			}

			/* now wait for connection */

			FD_ZERO(&socket_set);
			FD_SET(server_socket,&socket_set);
			high_socket_set=server_socket+1;
			if(startup->options&MAIL_OPT_ALLOW_POP3 
				&& pop3_socket!=INVALID_SOCKET) {
				FD_SET(pop3_socket,&socket_set);
				if(pop3_socket+1>high_socket_set)
					high_socket_set=pop3_socket+1;
			}
			if(startup->options&MAIL_OPT_USE_SUBMISSION_PORT 
				&& submission_socket!=INVALID_SOCKET) {
				FD_SET(submission_socket,&socket_set);
				if(submission_socket+1>high_socket_set)
					high_socket_set=submission_socket+1;
			}

			tv.tv_sec=startup->sem_chk_freq;
			tv.tv_usec=0;

			if((i=select(high_socket_set,&socket_set,NULL,NULL,&tv))<1) {
				if(i==0)
					continue;
				if(ERROR_VALUE==EINTR)
					lprintf(LOG_DEBUG,"%04d Mail Server listening interrupted",server_socket);
				else if(ERROR_VALUE == ENOTSOCK)
            		lprintf(LOG_NOTICE,"%04d Mail Server sockets closed",server_socket);
				else
					lprintf(LOG_WARNING,"%04d !ERROR %d selecting sockets",server_socket,ERROR_VALUE);
				continue;
			}

			if(server_socket!=INVALID_SOCKET && !terminate_server
				&& (FD_ISSET(server_socket,&socket_set) 
					|| (startup->options&MAIL_OPT_USE_SUBMISSION_PORT
						&& FD_ISSET(submission_socket,&socket_set)))) {

				client_addr_len = sizeof(client_addr);
				client_socket = accept(
					FD_ISSET(server_socket,&socket_set) ? server_socket:submission_socket
					,(struct sockaddr *)&client_addr
        			,&client_addr_len);

				if(client_socket == INVALID_SOCKET)
				{
#if 0	/* is this necessary still? */
					if(ERROR_VALUE == ENOTSOCK || ERROR_VALUE == EINVAL) {
            			lprintf(LOG_NOTICE,"%04d SMTP socket closed while listening"
							,server_socket);
						break;
					}
#endif
					lprintf(LOG_WARNING,"%04d SMTP !ERROR %d accepting connection"
						,FD_ISSET(server_socket,&socket_set) ? server_socket:submission_socket
						,ERROR_VALUE);
#ifdef _WIN32
					if(WSAGetLastError()==WSAENOBUFS)	/* recycle (re-init WinSock) on this error */
						break;
#endif
					continue;
				}
				if(startup->socket_open!=NULL)
					startup->socket_open(startup->cbdata,TRUE);
				stats.sockets++;

				if(trashcan(&scfg,inet_ntoa(client_addr.sin_addr),"ip-silent")) {
					mail_close_socket(client_socket);
					stats.connections_ignored++;
					continue;
				}

				if(active_clients.value>=startup->max_clients) {
					lprintf(LOG_WARNING,"%04d SMTP !MAXIMUM CLIENTS (%u) reached, access denied (%u total)"
						,client_socket, startup->max_clients, ++stats.connections_refused);
					sockprintf(client_socket,"421 Maximum active clients reached, please try again later.");
					mswait(3000);
					mail_close_socket(client_socket);
					continue;
				}

				l=1;

				if((i=ioctlsocket(client_socket, FIONBIO, &l))!=0) {
					lprintf(LOG_CRIT,"%04d SMTP !ERROR %d (%d) disabling blocking on socket"
						,client_socket, i, ERROR_VALUE);
					mail_close_socket(client_socket);
					continue;
				}

				if((smtp=malloc(sizeof(smtp_t)))==NULL) {
					lprintf(LOG_CRIT,"%04d SMTP !ERROR allocating %u bytes of memory for smtp_t"
						,client_socket, sizeof(smtp_t));
					mail_close_socket(client_socket);
					continue;
				}

				smtp->socket=client_socket;
				smtp->client_addr=client_addr;
				_beginthread(smtp_thread, 0, smtp);
				stats.connections_served++;
			}

			if(pop3_socket!=INVALID_SOCKET
				&& FD_ISSET(pop3_socket,&socket_set)) {

				client_addr_len = sizeof(client_addr);
				client_socket = accept(pop3_socket, (struct sockaddr *)&client_addr
        			,&client_addr_len);

				if(client_socket == INVALID_SOCKET)
				{
#if 0	/* is this necessary still? */
					if(ERROR_VALUE == ENOTSOCK || ERROR_VALUE == EINVAL) {
            			lprintf(LOG_NOTICE,"%04d POP3 socket closed while listening",pop3_socket);
						break;
					}
#endif
					lprintf(LOG_WARNING,"%04d POP3 !ERROR %d accepting connection"
						,pop3_socket, ERROR_VALUE);
#ifdef _WIN32
					if(WSAGetLastError()==WSAENOBUFS)	/* recycle (re-init WinSock) on this error */
						break;
#endif
					continue;
				}
				if(startup->socket_open!=NULL)
					startup->socket_open(startup->cbdata,TRUE);
				stats.sockets++;

				if(trashcan(&scfg,inet_ntoa(client_addr.sin_addr),"ip-silent")) {
					mail_close_socket(client_socket);
					stats.connections_ignored++;
					continue;
				}

				if(active_clients.value>=startup->max_clients) {
					lprintf(LOG_WARNING,"%04d POP3 !MAXIMUM CLIENTS (%u) reached, access denied (%u total)"
						,client_socket, startup->max_clients, ++stats.connections_refused);
					sockprintf(client_socket,"-ERR Maximum active clients reached, please try again later.");
					mswait(3000);
					mail_close_socket(client_socket);
					continue;
				}

				l=1;

				if((i=ioctlsocket(client_socket, FIONBIO, &l))!=0) {
					lprintf(LOG_CRIT,"%04d POP3 !ERROR %d (%d) disabling blocking on socket"
						,client_socket, i, ERROR_VALUE);
					sockprintf(client_socket,"-ERR System error, please try again later.");
					mswait(3000);
					mail_close_socket(client_socket);
					continue;
				}

				if((pop3=malloc(sizeof(pop3_t)))==NULL) {
					lprintf(LOG_CRIT,"%04d POP3 !ERROR allocating %u bytes of memory for pop3_t"
						,client_socket,sizeof(pop3_t));
					sockprintf(client_socket,"-ERR System error, please try again later.");
					mswait(3000);
					mail_close_socket(client_socket);
					continue;
				}

				pop3->socket=client_socket;
				pop3->client_addr=client_addr;

				_beginthread(pop3_thread, 0, pop3);
				stats.connections_served++;
			}
		}

		if(active_clients.value) {
			lprintf(LOG_DEBUG,"%04d Waiting for %d active clients to disconnect..."
				,server_socket, active_clients.value);
			start=time(NULL);
			while(active_clients.value) {
				if(startup->max_inactivity && time(NULL)-start>startup->max_inactivity) {
					lprintf(LOG_WARNING,"%04d !TIMEOUT (%u seconds) waiting for %d active clients"
						,server_socket, startup->max_inactivity, active_clients.value);
					break;
				}
				mswait(100);
			}
		}

		if(sendmail_running) {
			terminate_sendmail=TRUE;
			sem_post(&sendmail_wakeup_sem);
			mswait(100);
		}
		if(sendmail_running) {
			lprintf(LOG_DEBUG,"%04d Waiting for SendMail thread to terminate..."
				,server_socket);
			start=time(NULL);
			while(sendmail_running) {
				if(time(NULL)-start>TIMEOUT_THREAD_WAIT) {
					lprintf(LOG_WARNING,"%04d !TIMEOUT waiting for sendmail thread to terminate"
						,server_socket);
					break;
				}
				mswait(500);
			}
		}
		if(!sendmail_running) {
			while(sem_destroy(&sendmail_wakeup_sem)==-1 && errno==EBUSY) {
				mswait(1);
				sem_post(&sendmail_wakeup_sem);
			}
		}

		cleanup(0);

		if(!terminate_server) {
			lprintf(LOG_INFO,"Recycling server...");
			mswait(2000);
			if(startup->recycle!=NULL)
				startup->recycle(startup->cbdata);
		}

	} while(!terminate_server);

	protected_uint32_destroy(thread_count);
}
