/* writemsg.cpp */

/* Synchronet message creation routines */

/* $Id: writemsg.cpp,v 1.103 2013/05/07 08:49:57 rswindell Exp $ */

/****************************************************************************
 * @format.tab-size 4		(Plain Text/Source Code File Header)			*
 * @format.use-tabs true	(see http://www.synchro.net/ptsc_hdr.html)		*
 *																			*
 * Copyright 2013 Rob Swindell - http://www.synchro.net/copyright.html		*
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

#include "sbbs.h"
#include "wordwrap.h"

#define MAX_LINES		10000
#define MAX_LINE_LEN	82	/* not strictly enforced, mostly used as a multiplier */

const char *quote_fmt=" > %.*s\r\n";
void quotestr(char *str);

/****************************************************************************/
/* Returns temporary message text filename (for message/text editors)		*/
/****************************************************************************/
char* sbbs_t::msg_tmp_fname(int xedit, char* path, size_t len)
{
	safe_snprintf(path, len, "%sINPUT.MSG", cfg.temp_dir);

	if(xedit) {
		if(cfg.xedit[xedit-1]->misc&QUICKBBS)
			safe_snprintf(path, len, "%sMSGTMP", cfg.node_dir);	/* QuickBBS editors are dumb */
		if(cfg.xedit[xedit-1]->misc&XTRN_LWRCASE)
			strlwr(getfname(path));
	}

	return path;
}

/****************************************************************************/
/****************************************************************************/
char* sbbs_t::quotes_fname(int xedit, char *path, size_t len)
{
	safe_snprintf(path, len, "%sQUOTES.TXT", cfg.node_dir);
	if(xedit && cfg.xedit[xedit-1]->misc&XTRN_LWRCASE)
		strlwr(getfname(path));
	return path;
}

/****************************************************************************/
/****************************************************************************/
void sbbs_t::quotemsg(smbmsg_t* msg, int tails)
{
	char	fname[MAX_PATH+1];
	char*	buf;
	char*	wrapped=NULL;
	FILE*	fp;

	quotes_fname(useron.xedit,fname,sizeof(fname));
	removecase(fname);

	if((fp=fopen(fname,"w"))==NULL) {
		errormsg(WHERE,ERR_OPEN,fname,0);
		return; 
	}

	if((buf=smb_getmsgtxt(&smb,msg,tails)) != NULL) {
		strip_invalid_attr(buf);
		if(useron.xedit && (cfg.xedit[useron.xedit-1]->misc&QUOTEWRAP))
			wrapped=::wordwrap(buf, cols-4, cols-1, /* handle_quotes: */TRUE);
		if(wrapped!=NULL) {
			fputs(wrapped,fp);
			free(wrapped);
		} else
			fputs(buf,fp);
		smb_freemsgtxt(buf); 
	} else if(smb_getmsgdatlen(msg)>2)
		errormsg(WHERE,ERR_READ,smb.file,smb_getmsgdatlen(msg));
	fclose(fp);
}

/****************************************************************************/
/****************************************************************************/
int sbbs_t::process_edited_text(char* buf, FILE* stream, long mode, unsigned* lines, unsigned maxlines)
{
	unsigned i,l;
	int	len=0;

	for(l=i=0;buf[l] && i<maxlines;l++) {
		if((uchar)buf[l]==141 && useron.xedit
    		&& cfg.xedit[useron.xedit-1]->misc&QUICKBBS) {
			len+=fwrite(crlf,1,2,stream);
			i++;
			continue; 
		}
		/* Expand LF to CRLF? */
		if(buf[l]==LF && (!l || buf[l-1]!=CR) && useron.xedit
			&& cfg.xedit[useron.xedit-1]->misc&EXPANDLF) {
			len+=fwrite(crlf,1,2,stream);
			i++;
			continue; 
		}
		/* Strip FidoNet Kludge Lines? */
		if(buf[l]==CTRL_A && useron.xedit
			&& cfg.xedit[useron.xedit-1]->misc&STRIPKLUDGE) {
			while(buf[l] && buf[l]!=LF) 
				l++;
			if(buf[l]==0)
				break;
			continue;
		}
		if(!(mode&(WM_EMAIL|WM_NETMAIL|WM_EDIT))
			&& (!l || buf[l-1]==LF)
			&& buf[l]=='-' && buf[l+1]=='-' && buf[l+2]=='-'
			&& (buf[l+3]==' ' || buf[l+3]==TAB || buf[l+3]==CR))
			buf[l+1]='+';
		if(buf[l]==LF)
			i++;
		fputc(buf[l],stream); 
		len++;
	}

	if(buf[l])
		bprintf(text[NoMoreLines], i);

	if(lines!=NULL)
		*lines=i;
	return len;
}

/****************************************************************************/
/****************************************************************************/
int sbbs_t::process_edited_file(const char* src, const char* dest, long mode, unsigned* lines, unsigned maxlines)
{
	char*	buf;
	long	len;
	FILE*	fp;

	if((len=(long)flength(src))<1)
		return -1;

	if((buf=(char*)malloc(len+1))==NULL)
		return -2;

	if((fp=fopen(src,"rb"))==NULL)
		return -3;

	memset(buf,0,len+1);
	fread(buf,len,sizeof(char),fp);
	fclose(fp);

	if((fp=fopen(dest,"wb"))!=NULL) {
		len=process_edited_text(buf, fp, mode, lines, maxlines);
		fclose(fp);
	}
	free(buf);

	return len;
}

/****************************************************************************/
/* Creates a message (post or mail) using standard line editor. 'fname' is  */
/* is name of file to create, 'top' is a buffer to place at beginning of    */
/* message and 'title' is the title (70chars max) for the message.          */
/* 'dest' contains a text description of where the message is going.        */
/****************************************************************************/
bool sbbs_t::writemsg(const char *fname, const char *top, char *title, long mode, uint subnum
	,const char *dest, char** editor)
{
	char	str[256],quote[128],c,*buf,*p,*tp
				,useron_level;
	char	msgtmp[MAX_PATH+1];
	char 	tmp[512];
	int		i,j,file,linesquoted=0;
	long	length,qlen=0,qtime=0,ex_mode=0;
	int		max_title_len=LEN_TITLE;
	ulong	l;
	FILE*	stream;
	FILE*	fp;
	unsigned lines;

	useron_level=useron.level;

	if(editor!=NULL)
		*editor=NULL;

	if((buf=(char*)malloc(cfg.level_linespermsg[useron_level]*MAX_LINE_LEN))
		==NULL) {
		errormsg(WHERE,ERR_ALLOC,fname
			,cfg.level_linespermsg[useron_level]*MAX_LINE_LEN);
		return(false); 
	}

	if(mode&WM_NETMAIL ||
		(!(mode&(WM_EMAIL|WM_NETMAIL)) && cfg.sub[subnum]->misc&SUB_PNET))
		mode|=WM_NOTOP;

	msg_tmp_fname(useron.xedit, msgtmp, sizeof(msgtmp));

	if(mode&WM_QUOTE && !(useron.rest&FLAG('J'))
		&& ((mode&(WM_EMAIL|WM_NETMAIL) && cfg.sys_misc&SM_QUOTE_EM)
		|| (!(mode&(WM_EMAIL|WM_NETMAIL)) && (uint)subnum!=INVALID_SUB
			&& cfg.sub[subnum]->misc&SUB_QUOTE))) {

		/* Quote entire message to MSGTMP or INPUT.MSG */

		if(useron.xedit && cfg.xedit[useron.xedit-1]->misc&QUOTEALL) {
			quotes_fname(useron.xedit, str, sizeof(str));
			if((stream=fnopen(NULL,str,O_RDONLY))==NULL) {
				errormsg(WHERE,ERR_OPEN,str,O_RDONLY);
				free(buf);
				return(false); 
			}

			if((file=nopen(msgtmp,O_WRONLY|O_CREAT|O_TRUNC))==-1) {
				errormsg(WHERE,ERR_OPEN,msgtmp,O_WRONLY|O_CREAT|O_TRUNC);
				free(buf);
				fclose(stream);
				return(false); 
			}

			while(!feof(stream) && !ferror(stream)) {
				if(!fgets(str,sizeof(str),stream))
					break;
				quotestr(str);
				SAFEPRINTF2(tmp,quote_fmt,cols-4,str);
				write(file,tmp,strlen(tmp));
				linesquoted++; 
			}
			fclose(stream);
			close(file); 
		}

		/* Quote nothing to MSGTMP or INPUT.MSG automatically */

		else if(useron.xedit && cfg.xedit[useron.xedit-1]->misc&QUOTENONE)
			;

		else if(yesno(text[QuoteMessageQ])) {
			quotes_fname(useron.xedit, str, sizeof(str));
			if((stream=fnopen(&file,str,O_RDONLY))==NULL) {
				errormsg(WHERE,ERR_OPEN,str,O_RDONLY);
				free(buf);
				return(false); 
			}

			if((file=nopen(msgtmp,O_WRONLY|O_CREAT|O_TRUNC))==-1) {
				errormsg(WHERE,ERR_OPEN,msgtmp,O_WRONLY|O_CREAT|O_TRUNC);
				free(buf);
				fclose(stream);
				return(false); 
			}

			l=(long)ftell(stream);			/* l now points to start of message */

			while(online) {
				SAFEPRINTF(str,text[QuoteLinesPrompt],linesquoted ? "Done":"All");
				mnemonics(str);
				i=getstr(quote,10,K_UPPER);
				if(sys_status&SS_ABORT) {
					fclose(stream);
					close(file);
					free(buf);
					return(false); 
				}
				if(!i && linesquoted)
					break;
				if(!i || quote[0]=='A') {                   /* Quote all */
					fseek(stream,l,SEEK_SET);
					while(!feof(stream) && !ferror(stream)) {
						if(!fgets(str,sizeof(str),stream))
							break;
						quotestr(str);
						SAFEPRINTF2(tmp,quote_fmt,cols-4,str);
						write(file,tmp,strlen(tmp));
						linesquoted++; 
					}
					break; 
				}
				if(quote[0]=='L') {
					fseek(stream,l,SEEK_SET);
					i=1;
					CRLF;
					attr(LIGHTGRAY);
					while(!feof(stream) && !ferror(stream) && !msgabort()) {
						if(!fgets(str,sizeof(str),stream))
							break;
						quotestr(str);
						bprintf("%3d: %.74s\r\n",i,str);
						i++; 
					}
					continue; 
				}

				if(!isdigit(quote[0]))
					break;
				p=quote;
				while(p) {
					if(*p==',' || *p==' ')
						p++;
					i=atoi(p);
					if(!i)
						break;
					fseek(stream,l,SEEK_SET);
					j=1;
					while(!feof(stream) && !ferror(stream) && j<i) {
						if(!fgets(tmp,sizeof(tmp),stream))
							break;
						j++; /* skip beginning */
					}		
					tp=strchr(p,'-');   /* tp for temp pointer */
					if(tp) {		 /* range */
						i=atoi(tp+1);
						while(!feof(stream) && !ferror(stream) && j<=i) {
							if(!fgets(str,sizeof(str),stream))
								break;
							quotestr(str);
							SAFEPRINTF2(tmp,quote_fmt,cols-4,str);
							write(file,tmp,strlen(tmp));
							linesquoted++;
							j++; 
						} 
					}
					else {			/* one line */
						if(fgets(str,sizeof(str),stream)) {
							quotestr(str);
							SAFEPRINTF2(tmp,quote_fmt,cols-4,str);
							write(file,tmp,strlen(tmp));
							linesquoted++; 
						} 
					}
					p=strchr(p,',');
					// if(!p) p=strchr(p,' ');  02/05/96 huh?
				} 
			}

			fclose(stream);
			close(file); 
		} 
	}
	else {
		quotes_fname(useron.xedit, str, sizeof(str));
		removecase(str); 
	}

	if(!online || sys_status&SS_ABORT) {
		free(buf);
		return(false); 
	}

	if(!(mode&(WM_EXTDESC|WM_SUBJ_RO))) {
		if(mode&WM_FILE) {
#if 0
			max_title_len=12;	/* ToDo: implied 8.3 filename limit! */
#endif
			CRLF;
			bputs(text[Filename]); 
		}
		else {
#if 0
			max_title_len=LEN_TITLE;
			if(mode&WM_QWKNET
				|| (subnum!=INVALID_SUB 
					&& (cfg.sub[subnum]->misc&(SUB_QNET|SUB_INET|SUB_FIDO))==SUB_QNET))
				max_title_len=25;
#endif
			bputs(text[SubjectPrompt]); 
		}
		if(!getstr(title,max_title_len,mode&WM_FILE ? K_LINE : K_LINE|K_EDIT|K_AUTODEL)
			&& useron_level && useron.logons) {
			free(buf);
			return(false); 
		}
		if(!(mode&(WM_EMAIL|WM_NETMAIL)) && cfg.sub[subnum]->misc&SUB_QNET
			&& !SYSOP
			&& (!stricmp(title,"DROP") || !stricmp(title,"ADD")
			|| !strnicmp(dest,"SBBS",4))) {
			free(buf);   /* Users can't post DROP or ADD in QWK netted subs */
			return(false); /* or messages to "SBBS" */
		}
	}

	if(!online || sys_status&SS_ABORT) {
		free(buf);
		return(false); 
	}

	smb.subnum = subnum;	/* Allow JS msgeditors to use bbs.smb_sub* */

	if(console&CON_RAW_IN) {
		bprintf(text[EnterMsgNowRaw]
			,(ulong)cfg.level_linespermsg[useron_level]*MAX_LINE_LEN);
		if(top[0] && !(mode&WM_NOTOP)) {
			strcpy((char *)buf,top);
			strcat((char *)buf,crlf);
			l=strlen((char *)buf); 
		}
		else
			l=0;
		while(l<(ulong)(cfg.level_linespermsg[useron_level]*MAX_LINE_LEN)) {
			c=getkey(0);
			if(sys_status&SS_ABORT) {  /* Ctrl-C */
				free(buf);
				return(false); 
			}
			if((c==ESC || c==CTRL_A) && useron.rest&FLAG('A')) /* ANSI restriction */
				continue;
			if(c==BEL && useron.rest&FLAG('B'))   /* Beep restriction */
				continue;
			if(!(console&CON_RAW_IN))	/* Ctrl-Z was hit */
				break;
			outchar(c);
			buf[l++]=c; 
		}
		buf[l]=0;
		if(l==(ulong)cfg.level_linespermsg[useron_level]*MAX_LINE_LEN)
			bputs(text[OutOfBytes]); 
	}


	else if(useron.xedit) {

		if(editor!=NULL)
			*editor=cfg.xedit[useron.xedit-1]->name;

		editor_inf(useron.xedit,dest,title,mode,subnum);
		if(cfg.xedit[useron.xedit-1]->type) {
			gettimeleft();
			xtrndat(useron.alias,cfg.node_dir,cfg.xedit[useron.xedit-1]->type
 			   ,timeleft,cfg.xedit[useron.xedit-1]->misc); 
		}

		if(cfg.xedit[useron.xedit-1]->misc&XTRN_STDIO) {
			ex_mode|=EX_STDIO;
			if(cfg.xedit[useron.xedit-1]->misc&WWIVCOLOR)
				ex_mode|=EX_WWIV; 
		}
		if(cfg.xedit[useron.xedit-1]->misc&XTRN_NATIVE)
			ex_mode|=EX_NATIVE;
		if(cfg.xedit[useron.xedit-1]->misc&XTRN_SH)
			ex_mode|=EX_SH;

		if(!linesquoted)
			removecase(msgtmp);
		else {
			qlen=(long)flength(msgtmp);
			qtime=(long)fdate(msgtmp); 
		}

		CLS;
		rioctl(IOCM|PAUSE|ABORT);
		external(cmdstr(cfg.xedit[useron.xedit-1]->rcmd,msgtmp,nulstr,NULL),ex_mode,cfg.node_dir);
		rioctl(IOSM|PAUSE|ABORT); 

		checkline();
		if(!fexistcase(msgtmp) || !online
			|| (linesquoted && qlen==flength(msgtmp) && qtime==fdate(msgtmp))) {
			free(buf);
			return(false); 
		}
		SAFEPRINTF(str,"%sRESULT.ED",cfg.node_dir);
		if(!(mode&(WM_EXTDESC|WM_FILE|WM_SUBJ_RO))
			&& !(cfg.xedit[useron.xedit-1]->misc&QUICKBBS) 
			&& fexistcase(str)) {
			if((fp=fopen(str,"r")) != NULL) {
				fgets(str,sizeof(str),fp);
				fgets(str,sizeof(str),fp);
				truncsp(str);
				safe_snprintf(title,max_title_len,"%s",str);
				fclose(fp);
			}
		}

		buf[0]=0;
		if(!(mode&WM_NOTOP))
			strcpy((char *)buf,top);
		if((file=nopen(msgtmp,O_RDONLY))==-1) {
			errormsg(WHERE,ERR_OPEN,msgtmp,O_RDONLY);
			free(buf);
			return(false); 
		}
		length=(long)filelength(file);
		l=strlen((char *)buf);	  /* reserve space for top and terminating null */
		/* truncate if too big */
		if(length>(long)((cfg.level_linespermsg[useron_level]*MAX_LINE_LEN)-(l+1))) {
			length=(cfg.level_linespermsg[useron_level]*MAX_LINE_LEN)-(l+1);
			bputs(text[OutOfBytes]); 
		}
		lread(file,buf+l,length);
		close(file);
		// remove(msgtmp); 	   /* no need to save the temp input file */
		buf[l+length]=0; 
	}
	else {
		buf[0]=0;
		if(linesquoted) {
			if((file=nopen(msgtmp,O_RDONLY))!=-1) {
				length=(long)filelength(file);
				l=length>(cfg.level_linespermsg[useron_level]*MAX_LINE_LEN)-1
					? (cfg.level_linespermsg[useron_level]*MAX_LINE_LEN)-1 : length;
				lread(file,buf,l);
				buf[l]=0;
				close(file);
				// remove(msgtmp);
			} 
		}
		if(!(msgeditor((char *)buf,mode&WM_NOTOP ? nulstr : top,title))) {
			free(buf);	/* Assertion here Dec-17-2003, think I fixed in block above (rev 1.52) */
			return(false); 
		} 
	}

	now=time(NULL);
	bputs(text[Saving]);
	if((stream=fnopen(NULL,fname,O_WRONLY|O_CREAT|O_TRUNC))==NULL) {
		errormsg(WHERE,ERR_OPEN,fname,O_WRONLY|O_CREAT|O_TRUNC);
		free(buf);
		return(false); 
	}
	l=process_edited_text(buf,stream,mode,&lines,cfg.level_linespermsg[useron_level]);

	/* Signature file */
	if((subnum==INVALID_SUB && cfg.msg_misc&MM_EMAILSIG)
		|| (subnum!=INVALID_SUB && !(cfg.sub[subnum]->misc&SUB_NOUSERSIG))) {
		SAFEPRINTF2(str,"%suser/%04u.sig",cfg.data_dir,useron.number);
		FILE* sig;
		if(fexist(str) && (sig=fopen(str,"r"))!=NULL) {
			while(!feof(sig)) {
				if(!fgets(str,sizeof(str),sig))
					break;
				truncsp(str);
				l+=fprintf(stream,"%s\r\n",str);
				lines++;		/* line counter */
			}
			fclose(sig);
		}
	}

	fclose(stream);
	free((char *)buf);
	bprintf(text[SavedNBytes],l,lines);
	return(true);
}

/****************************************************************************/
/* Modify 'str' to for quoted format. Remove ^A codes, etc.                 */
/****************************************************************************/
void quotestr(char *str)
{
	truncsp(str);
	remove_ctrl_a(str,str);
}

/****************************************************************************/
/****************************************************************************/
void sbbs_t::editor_inf(int xeditnum, const char *dest, const char *title, long mode
	,uint subnum)
{
	char str[MAX_PATH+1];
	char tmp[32];
	int file;

	xeditnum--;

	if(cfg.xedit[xeditnum]->misc&QUICKBBS) {
		strcpy(tmp,"MSGINF");
		if(cfg.xedit[xeditnum]->misc&XTRN_LWRCASE)
			strlwr(tmp);
		SAFEPRINTF2(str,"%s%s",cfg.node_dir,tmp);
		if((file=nopen(str,O_WRONLY|O_CREAT|O_TRUNC))==-1) {
			errormsg(WHERE,ERR_OPEN,str,O_WRONLY|O_CREAT|O_TRUNC);
			return; 
		}
		safe_snprintf(str,sizeof(str),"%s\r\n%s\r\n%s\r\n%u\r\n%s\r\n%s\r\n"
			,(subnum!=INVALID_SUB && cfg.sub[subnum]->misc&SUB_NAME) ? useron.name
				: useron.alias
				,dest,title,1
				,mode&WM_NETMAIL ? "NetMail"
				:mode&WM_EMAIL ? "Electronic Mail"
				:subnum==INVALID_SUB ? nulstr
				:cfg.sub[subnum]->sname
			,mode&WM_PRIVATE ? "YES":"NO");
		write(file,str,strlen(str));
		close(file); 
	}
	else {
		SAFEPRINTF(str,"%sRESULT.ED",cfg.node_dir);
		removecase(str);
		strcpy(tmp,"EDITOR.INF");
		if(cfg.xedit[xeditnum]->misc&XTRN_LWRCASE)
			strlwr(tmp);
		SAFEPRINTF2(str,"%s%s",cfg.node_dir,tmp);
		if((file=nopen(str,O_WRONLY|O_CREAT|O_TRUNC))==-1) {
			errormsg(WHERE,ERR_OPEN,str,O_WRONLY|O_CREAT|O_TRUNC);
			return; 
		}
		safe_snprintf(str,sizeof(str),"%s\r\n%s\r\n%u\r\n%s\r\n%s\r\n%u\r\n"
			,title,dest,useron.number
			,(subnum!=INVALID_SUB && cfg.sub[subnum]->misc&SUB_NAME) ? useron.name
			: useron.alias
			,useron.name,useron.level);
		write(file,str,strlen(str));
		close(file); 
	}
}



/****************************************************************************/
/* Removes from file 'str' every LF terminated line that starts with 'str2' */
/* That is divisable by num. Function skips first 'skip' number of lines    */
/****************************************************************************/
void sbbs_t::removeline(char *str, char *str2, char num, char skip)
{
	char*	buf;
    char    slen;
    int     i,file;
	long	l=0,flen;
    FILE    *stream;

	if((file=nopen(str,O_RDONLY))==-1) {
		errormsg(WHERE,ERR_OPEN,str,O_RDONLY);
		return; 
	}
	flen=(long)filelength(file);
	slen=strlen(str2);
	if((buf=(char *)malloc(flen))==NULL) {
		close(file);
		errormsg(WHERE,ERR_ALLOC,str,flen);
		return; 
	}
	if(lread(file,buf,flen)!=flen) {
		close(file);
		errormsg(WHERE,ERR_READ,str,flen);
		free(buf);
		return; 
	}
	close(file);
	if((stream=fnopen(&file,str,O_WRONLY|O_TRUNC))==NULL) {
		close(file);
		errormsg(WHERE,ERR_OPEN,str,O_WRONLY|O_TRUNC);
		free(buf);
		return; 
	}
	for(i=0;l<flen && i<skip;l++) {
		fputc(buf[l],stream);
		if(buf[l]==LF)
			i++; 
	}
	while(l<flen) {
		if(!strncmp((char *)buf+l,str2,slen)) {
			for(i=0;i<num && l<flen;i++) {
				while(l<flen && buf[l]!=LF) l++;
				l++; 
			} 
		}
		else {
			for(i=0;i<num && l<flen;i++) {
				while(l<flen && buf[l]!=LF) fputc(buf[l++],stream);
				fputc(buf[l++],stream); 
			} 
		} 
	}
	fclose(stream);
	free((char *)buf);
}

/*****************************************************************************/
/* The Synchronet editor.                                                    */
/* Returns the number of lines edited.                                       */
/*****************************************************************************/
ulong sbbs_t::msgeditor(char *buf, const char *top, char *title)
{
	int		i,j,line,lines=0,maxlines;
	char	strin[256],**str,done=0;
	char 	tmp[512];
	char	path[MAX_PATH+1];
    ulong	l,m;

	rioctl(IOCM|ABORT);
	rioctl(IOCS|ABORT); 

	maxlines=cfg.level_linespermsg[useron.level];

	if((str=(char **)malloc(sizeof(char *)*(maxlines+1)))==NULL) {
		errormsg(WHERE,ERR_ALLOC,"msgeditor",sizeof(char *)*(maxlines+1));
		return(0); 
	}
	m=strlen(buf);
	l=0;
	while(l<m && lines<maxlines) {
		msgabort(); /* to allow pausing */
		if((str[lines]=(char *)malloc(MAX_LINE_LEN))==NULL) {
			errormsg(WHERE,ERR_ALLOC,nulstr,MAX_LINE_LEN);
			for(i=0;i<lines;i++)
				free(str[i]);
			free(str);
			rioctl(IOSM|ABORT);
			return(0); 
		}
		for(i=0;i<79 && l<m;i++,l++) {
			if(buf[l]==CR) {
				l+=2;
				break; 
			}
			if(buf[l]==TAB) {
				if(!(i%8))                  /* hard-coded tabstop of 8 */
					str[lines][i++]=' ';     /* for expansion */
				while(i%8 && i<79)
					str[lines][i++]=' ';
				i--;
				/***
				bprintf("\r\nMessage editor: Expanded tab on line #%d",lines+1);
				***/ }
			else str[lines][i]=buf[l]; 
		}
		if(i==79) {
			if(buf[l]==CR)
				l+=2;
			else
				bprintf("\r\nMessage editor: Split line #%d",lines+1); 
		}
		str[lines][i]=0;
		lines++; 
	}
	if(lines)
		bprintf("\r\nMessage editor: Read in %d lines\r\n",lines);
	bprintf(text[EnterMsgNow],maxlines);

	SAFEPRINTF(path,"%smenu/msgtabs.*", cfg.text_dir);
	if(fexist(path))
		menu("msgtabs");
	else {
		for(i=0;i<79;i++) {
			if(i%EDIT_TABSIZE || !i)
				outchar('-');
			else 
				outchar('+');
		}
		CRLF;
	}
	putmsg(top,P_SAVEATR|P_NOATCODES);
	for(line=0;line<lines && !msgabort();line++) { /* display lines in buf */
		putmsg(str[line],P_SAVEATR|P_NOATCODES);
		cleartoeol();  /* delete to end of line */
		CRLF; 
	}
	SYNC;
	rioctl(IOSM|ABORT);
	while(online && !done) {
		checkline();
		if(line==lines) {
			if((str[line]=(char *)malloc(MAX_LINE_LEN))==NULL) {
				errormsg(WHERE,ERR_ALLOC,nulstr,MAX_LINE_LEN);
				for(i=0;i<lines;i++)
					free(str[i]);
				free(str);
				return(0); 
			}
			str[line][0]=0; 
		}
		if(line>(maxlines-10)) {
			if(line==maxlines)
				bprintf(text[NoMoreLines],line);
			else
				bprintf(text[OnlyNLinesLeft],maxlines-line); 
		}
		strcpy(strin,str[line]);
		do {
			if(!line)
				outchar(CR);
			getstr(strin,79,K_WRAP|K_MSG|K_EDIT);
			} while(console&CON_UPARROW && !line);

		if(sys_status&SS_ABORT) {
			if(line==lines)
				free(str[line]);
			continue; 
		}
		if(strin[0]=='/' && strlen(strin)<8) {
			if(!stricmp(strin,"/DEBUG") && SYSOP) {
				if(line==lines)
					free(str[line]);
				bprintf("\r\nline=%d lines=%d rows=%d\r\n",line,lines,rows);
				continue; 
			}
			else if(!stricmp(strin,"/ABT")) {
				if(line==lines) 		/* delete a line */
					free(str[line]);
				for(i=0;i<lines;i++)
					free(str[i]);
				free(str);
				return(0); 
			}
			else if(toupper(strin[1])=='D') {
				if(line==lines)         /* delete a line */
					free(str[line]);
				if(!lines)
					continue;
				i=atoi(strin+2)-1;
				if(i==-1)   /* /D means delete last line */
					i=lines-1;
				if(i>=lines || i<0)
					bputs(text[InvalidLineNumber]);
				else {
					free(str[i]);
					lines--;
					while(i<lines) {
						str[i]=str[i+1];
						i++; 
					}
					if(line>lines)
						line=lines; 
				}
				continue; 
			}
			else if(toupper(strin[1])=='I') {
				if(line==lines)         /* insert a line before number x */
					free(str[line]);
				if(line==maxlines || !lines)
					continue;
				i=atoi(strin+2)-1;
				if(i==-1)
					i=lines-1;
				if(i>=lines || i<0)
					bputs(text[InvalidLineNumber]);
				else {
					for(line=lines;line>i;line--)   /* move the pointers */
						str[line]=str[line-1];
					if((str[i]=(char *)malloc(MAX_LINE_LEN))==NULL) {
						errormsg(WHERE,ERR_ALLOC,nulstr,MAX_LINE_LEN);
						for(i=0;i<lines;i++)
							free(str[i]);
						free(str);
						return(0); 
					}
					str[i][0]=0;
					line=++lines; 
				}
				continue; 
			}
			else if(toupper(strin[1])=='E') {
				if(line==lines)         /* edit a line */
					free(str[line]);
				if(!lines)
					continue;
				i=atoi(strin+2)-1;
				j=K_MSG|K_EDIT; /* use j for the getstr mode */
				if(i==-1) { /* /E means edit last line */
					i=lines-1;
					j|=K_WRAP;	/* wrap when editing last line */
				}    
				if(i>=lines || i<0)
					bputs(text[InvalidLineNumber]);
				else
					getstr(str[i],79,j);
				continue; 
			}
			else if(!stricmp(strin,"/CLR")) {
				bputs(text[MsgCleared]);
				if(line!=lines)
					lines--;
				for(i=0;i<=lines;i++)
					free(str[i]);
				line=0;
				lines=0;
				putmsg(top,P_SAVEATR|P_NOATCODES);
				continue; 
			}
			else if(toupper(strin[1])=='L') {   /* list message */
				if(line==lines)
					free(str[line]);
				if(lines && text[WithLineNumbersQ][0])
					i=!noyes(text[WithLineNumbersQ]);
				else
					i=0;
				CRLF;
				attr(LIGHTGRAY);
				putmsg(top,P_SAVEATR|P_NOATCODES);
				if(!lines) {
					continue; 
				}
				j=atoi(strin+2);
				if(j) j--;  /* start from line j */
				while(j<lines && !msgabort()) {
					if(i) { /* line numbers */
						SAFEPRINTF2(tmp,"%3d: %-.74s",j+1,str[j]);
						putmsg(tmp,P_SAVEATR|P_NOATCODES); 
					}
					else
						putmsg(str[j],P_SAVEATR|P_NOATCODES);
					cleartoeol();  /* delete to end of line */
					CRLF;
					j++; 
				}
				SYNC;
				continue; 
			}
			else if(!stricmp(strin,"/S")) { /* Save */
				if(line==lines)
					free(str[line]);
				done=1;
				continue;}
			else if(!stricmp(strin,"/T")) { /* Edit title/subject */
				if(line==lines)
					free(str[line]);
				if(title[0]) {
					bputs(text[SubjectPrompt]);
					getstr(title,LEN_TITLE,K_LINE|K_EDIT|K_AUTODEL);
					SYNC;
					CRLF; 
				}
				continue; 
			}
			else if(!stricmp(strin,"/?")) {
				if(line==lines)
					free(str[line]);
				menu("editor"); /* User Editor Commands */
				SYNC;
				continue; 
			}
			else if(!stricmp(strin,"/ATTR"))    {
				if(line==lines)
					free(str[line]);
				menu("attr");   /* User ANSI Commands */
				SYNC;
				continue; 
			} 
		}
		strcpy(str[line],strin);
		if(line<maxlines)
			line++;
		else
			free(str[line]);
		if(line>lines)
			lines++;
		if(console&CON_UPARROW) {
			outchar(CR);
			cursor_up();
			cleartoeol();
			line-=2; 
		}
		}
	if(!online) {
		for(i=0;i<lines;i++)
			free(str[i]);
		free(str);
		return(0); 
	}
	strcpy(buf,top);
	for(i=0;i<lines;i++) {
		strcat(buf,str[i]);
		strcat(buf,crlf);
		free(str[i]); 
	}
	free(str);
	return(lines);
}


/****************************************************************************/
/* Edits an existing file or creates a new one in MSG format                */
/****************************************************************************/
bool sbbs_t::editfile(char *fname, bool msg)
{
	char *buf,path[MAX_PATH+1];
	char msgtmp[MAX_PATH+1];
	char str[MAX_PATH+1];
    int file;
	long length,maxlines,l,mode=0;
	FILE*	stream;
	unsigned lines;

	if(msg)
		maxlines=cfg.level_linespermsg[useron.level];
	else
		maxlines=MAX_LINES;
	quotes_fname(useron.xedit, path, sizeof(path));
	removecase(path);

	if(useron.xedit) {

		SAFECOPY(path,fname);

		msg_tmp_fname(useron.xedit, msgtmp, sizeof(msgtmp));
		if(stricmp(msgtmp,path)) {
			removecase(msgtmp);
			if(fexistcase(path))
				fcopy(path, msgtmp);
		}

		editor_inf(useron.xedit,fname,nulstr,0,INVALID_SUB);
		if(cfg.xedit[useron.xedit-1]->misc&XTRN_NATIVE)
			mode|=EX_NATIVE;
		if(cfg.xedit[useron.xedit-1]->misc&XTRN_SH)
			mode|=EX_SH;
		if(cfg.xedit[useron.xedit-1]->misc&XTRN_STDIO) {
			mode|=EX_STDIO;
			if(cfg.xedit[useron.xedit-1]->misc&WWIVCOLOR)
				mode|=EX_WWIV; 
		}
		CLS;
		rioctl(IOCM|PAUSE|ABORT);
		if(external(cmdstr(cfg.xedit[useron.xedit-1]->rcmd,msgtmp,nulstr,NULL),mode,cfg.node_dir)!=0)
			return false;
		l=process_edited_file(msgtmp, path, /* mode: */WM_EDIT, &lines,maxlines);
		if(l>0) {
			SAFEPRINTF4(str,"%s created or edited file: %s (%u bytes, %u lines)"
				,useron.alias, path, l, lines);
			logline(LOG_NOTICE,nulstr,str);
		}
		rioctl(IOSM|PAUSE|ABORT); 
		return true; 
	}
	if((buf=(char *)malloc(maxlines*MAX_LINE_LEN))==NULL) {
		errormsg(WHERE,ERR_ALLOC,nulstr,maxlines*MAX_LINE_LEN);
		return false; 
	}
	if((file=nopen(fname,O_RDONLY))!=-1) {
		length=(long)filelength(file);
		if(length>(long)maxlines*MAX_LINE_LEN) {
			close(file);
			free(buf); 
			attr(cfg.color[clr_err]);
			bprintf("\7\r\nFile size (%lu bytes) is larger than %lu (maxlines: %lu).\r\n"
				,length, (ulong)maxlines*MAX_LINE_LEN, maxlines);
			return false;
		}
		if(read(file,buf,length)!=length) {
			close(file);
			free(buf);
			errormsg(WHERE,ERR_READ,fname,length);
			return false; 
		}
		buf[length]=0;
		close(file); 
	}
	else {
		buf[0]=0;
		bputs(text[NewFile]); 
	}
	if(!msgeditor(buf,nulstr,nulstr)) {
		free(buf);
		return false; 
	}
	bputs(text[Saving]);
	if((stream=fnopen(NULL,fname,O_CREAT|O_WRONLY|O_TRUNC))==NULL) {
		errormsg(WHERE,ERR_OPEN,fname,O_CREAT|O_WRONLY|O_TRUNC);
		free(buf);
		return false; 
	}
	l=process_edited_text(buf,stream,/* mode: */WM_EDIT,&lines,maxlines);
	bprintf(text[SavedNBytes],l,lines);
	fclose(stream);
	free(buf);
	SAFEPRINTF4(str,"%s created or edited file: %s (%u bytes, %u lines)"
		,useron.alias, fname, l, lines);
	logline(nulstr,str);
	return true;
}

/*************************/
/* Copy file attachments */
/*************************/
void sbbs_t::copyfattach(uint to, uint from, char *title)
{
	char str[128],str2[128],str3[128],*tp,*sp,*p;

	strcpy(str,title);
	tp=str;
	while(1) {
		p=strchr(tp,' ');
		if(p) *p=0;
		sp=strrchr(tp,'/');              /* sp is slash pointer */
		if(!sp) sp=strrchr(tp,'\\');
		if(sp) tp=sp+1;
		SAFEPRINTF3(str2,"%sfile/%04u.in/%s"  /* str2 is path/fname */
			,cfg.data_dir,to,tp);
		SAFEPRINTF3(str3,"%sfile/%04u.in/%s"  /* str2 is path/fname */
			,cfg.data_dir,from,tp);
		if(strcmp(str2,str3))
			mv(str3,str2,1);
		if(!p)
			break;
		tp=p+1; 
	}
}


/****************************************************************************/
/* Forwards mail (fname) to usernumber                                      */
/* Called from function readmail											*/
/****************************************************************************/
void sbbs_t::forwardmail(smbmsg_t *msg, int usernumber)
{
	char		str[256],touser[128];
	char 		tmp[512];
	int			i;
	node_t		node;
	msghdr_t	hdr=msg->hdr;
	idxrec_t	idx=msg->idx;
	time32_t	now32;

	if(useron.etoday>=cfg.level_emailperday[useron.level] && !SYSOP && !(useron.exempt&FLAG('M'))) {
		bputs(text[TooManyEmailsToday]);
		return; 
	}
	if(useron.rest&FLAG('F')) {
		bputs(text[R_Forward]);
		return; 
	}
	if(usernumber==1 && useron.rest&FLAG('S')) {
		bprintf(text[R_Feedback],cfg.sys_op);
		return; 
	}
	if(usernumber!=1 && useron.rest&FLAG('E')) {
		bputs(text[R_Email]);
		return; 
	}

	msg->idx.attr&=~(MSG_READ|MSG_DELETE);
	msg->hdr.attr=msg->idx.attr;


	smb_hfield_str(msg,SENDER,useron.alias);
	SAFEPRINTF(str,"%u",useron.number);
	smb_hfield_str(msg,SENDEREXT,str);

	/* Security logging */
	msg_client_hfields(msg,&client);
	smb_hfield_str(msg,SENDERSERVER,startup->host_name);

	username(&cfg,usernumber,touser);
	smb_hfield_str(msg,RECIPIENT,touser);
	SAFEPRINTF(str,"%u",usernumber);
	smb_hfield_str(msg,RECIPIENTEXT,str);
	msg->idx.to=usernumber;

	now32=time32(NULL);
	smb_hfield(msg,FORWARDED,sizeof(time32_t),&now32);


	if((i=smb_open_da(&smb))!=SMB_SUCCESS) {
		errormsg(WHERE,ERR_OPEN,smb.file,i,smb.last_error);
		return; 
	}
	if((i=smb_incmsg_dfields(&smb,msg,1))!=SMB_SUCCESS) {
		errormsg(WHERE,ERR_WRITE,smb.file,i);
		return; 
	}
	smb_close_da(&smb);


	if((i=smb_addmsghdr(&smb,msg,SMB_SELFPACK))!=SMB_SUCCESS) {
		errormsg(WHERE,ERR_WRITE,smb.file,i,smb.last_error);
		smb_freemsg_dfields(&smb,msg,1);
		return; 
	}

	if(msg->hdr.auxattr&MSG_FILEATTACH)
		copyfattach(usernumber,useron.number,msg->subj);

	bprintf(text[Forwarded],username(&cfg,usernumber,str),usernumber);
	SAFEPRINTF3(str,"%s forwarded mail to %s #%d"
		,useron.alias
		,username(&cfg,usernumber,tmp)
		,usernumber);
	logline("E",str);
	msg->idx=idx;
	msg->hdr=hdr;


	if(usernumber==1) {
		useron.fbacks++;
		logon_fbacks++;
		putuserrec(&cfg,useron.number,U_FBACKS,5,ultoa(useron.fbacks,tmp,10)); 
	}
	else {
		useron.emails++;
		logon_emails++;
		putuserrec(&cfg,useron.number,U_EMAILS,5,ultoa(useron.emails,tmp,10)); 
	}
	useron.etoday++;
	putuserrec(&cfg,useron.number,U_ETODAY,5,ultoa(useron.etoday,tmp,10));

	for(i=1;i<=cfg.sys_nodes;i++) { /* Tell user, if online */
		getnodedat(i,&node,0);
		if(node.useron==usernumber && !(node.misc&NODE_POFF)
			&& (node.status==NODE_INUSE || node.status==NODE_QUIET)) {
			SAFEPRINTF2(str,text[EmailNodeMsg],cfg.node_num,useron.alias);
			putnmsg(&cfg,i,str);
			break; 
		} 
	}
	if(i>cfg.sys_nodes) {	/* User wasn't online, so leave short msg */
		SAFEPRINTF(str,text[UserSentYouMail],useron.alias);
		putsmsg(&cfg,usernumber,str); 
	}
}

/****************************************************************************/
/* Auto-Message Routine ('A' from the main menu)                            */
/****************************************************************************/
void sbbs_t::automsg()
{
    char	str[256],buf[300],anon=0;
	char 	tmp[512];
	char	automsg[MAX_PATH+1];
    int		file;
	time_t	now=time(NULL);

	SAFEPRINTF(automsg,"%smsgs/auto.msg",cfg.data_dir);
	while(online) {
		SYNC;
		mnemonics(text[AutoMsg]);
		switch(getkeys("RWQ",0)) {
			case 'R':
				printfile(automsg,P_NOABORT|P_NOATCODES);
				break;
			case 'W':
				if(useron.rest&FLAG('W')) {
					bputs(text[R_AutoMsg]);
					break; 
				}
				action=NODE_AMSG;
				SYNC;
				bputs("\r\n3 lines:\r\n");
				if(!getstr(str,68,K_WRAP|K_MSG))
					break;
				strcpy(buf,str);
				strcat(buf,"\r\n          ");
				getstr(str,68,K_WRAP|K_MSG);
				strcat(buf,str);
				strcat(buf,"\r\n          ");
				getstr(str,68,K_MSG);
				strcat(str,crlf);
				strcat(buf,str);
				if(yesno(text[OK])) {
					if(useron.exempt&FLAG('A')) {
						if(!noyes(text[AnonymousQ]))
							anon=1; 
					}
					if((file=nopen(automsg,O_WRONLY|O_CREAT|O_TRUNC))==-1) {
						errormsg(WHERE,ERR_OPEN,automsg,O_WRONLY|O_CREAT|O_TRUNC);
						return; 
					}
					if(anon)
						SAFEPRINTF(tmp,"%.80s",text[Anonymous]);
					else
						SAFEPRINTF2(tmp,"%s #%d",useron.alias,useron.number);
					SAFEPRINTF2(str,text[AutoMsgBy],tmp,timestr(now));
					strcat(str,"          ");
					write(file,str,strlen(str));
					write(file,buf,strlen(buf));
					close(file); 
				}
				break;
			case 'Q':
				return; 
		} 
	}
}

/****************************************************************************/
/* Edits messages															*/
/****************************************************************************/
void sbbs_t::editmsg(smbmsg_t *msg, uint subnum)
{
	char	buf[SDT_BLOCK_LEN];
	char	msgtmp[MAX_PATH+1];
	uint16_t	xlat;
	int 	file,i,j,x;
	long	length,offset;
	FILE	*instream;

	if(!msg->hdr.total_dfields)
		return;

	msg_tmp_fname(useron.xedit, msgtmp, sizeof(msgtmp));
	removecase(msgtmp);
	msgtotxt(msg,msgtmp,0,1);
	if(!editfile(msgtmp, /* msg: */true))
		return;
	length=(long)flength(msgtmp);
	if(length<1L)
		return;

	length+=2;	 /* +2 for translation string */

	if((i=smb_locksmbhdr(&smb))!=SMB_SUCCESS) {
		errormsg(WHERE,ERR_LOCK,smb.file,i,smb.last_error);
		return; 
	}

	if((i=smb_getstatus(&smb))!=SMB_SUCCESS) {
		errormsg(WHERE,ERR_READ,smb.file,i,smb.last_error);
		return; 
	}

	if(!(smb.status.attr&SMB_HYPERALLOC)) {
		if((i=smb_open_da(&smb))!=SMB_SUCCESS) {
			errormsg(WHERE,ERR_OPEN,smb.file,i,smb.last_error);
			return; 
		}
		if((i=smb_freemsg_dfields(&smb,msg,1))!=SMB_SUCCESS)
			errormsg(WHERE,ERR_WRITE,smb.file,i,smb.last_error); 
	}

	msg->dfield[0].type=TEXT_BODY;				/* Make one single data field */
	msg->dfield[0].length=length;
	msg->dfield[0].offset=0;
	for(x=1;x<msg->hdr.total_dfields;x++) { 	/* Clear the other data fields */
		msg->dfield[x].type=UNUSED; 			/* so we leave the header length */
		msg->dfield[x].length=0;				/* unchanged */
		msg->dfield[x].offset=0; 
	}


	if(smb.status.attr&SMB_HYPERALLOC)
		offset=smb_hallocdat(&smb); 
	else {
		if((subnum!=INVALID_SUB && cfg.sub[subnum]->misc&SUB_FAST)
			|| (subnum==INVALID_SUB && cfg.sys_misc&SM_FASTMAIL))
			offset=smb_fallocdat(&smb,length,1);
		else
			offset=smb_allocdat(&smb,length,1);
		smb_close_da(&smb); 
	}

	msg->hdr.offset=offset;
	if((file=open(msgtmp,O_RDONLY|O_BINARY))==-1
		|| (instream=fdopen(file,"rb"))==NULL) {
		smb_unlocksmbhdr(&smb);
		smb_freemsgdat(&smb,offset,length,1);
		errormsg(WHERE,ERR_OPEN,msgtmp,O_RDONLY|O_BINARY);
		return; 
	}

	setvbuf(instream,NULL,_IOFBF,2*1024);
	fseek(smb.sdt_fp,offset,SEEK_SET);
	xlat=XLAT_NONE;
	fwrite(&xlat,2,1,smb.sdt_fp);
	x=SDT_BLOCK_LEN-2;				/* Don't read/write more than 255 */
	while(!feof(instream)) {
		memset(buf,0,x);
		j=fread(buf,1,x,instream);
		if(j<1)
			break;
		if(j>1 && (j!=x || feof(instream)) && buf[j-1]==LF && buf[j-2]==CR)
			buf[j-1]=buf[j-2]=0;	/* Convert to NULL */
		fwrite(buf,j,1,smb.sdt_fp);
		x=SDT_BLOCK_LEN; 
	}
	fflush(smb.sdt_fp);
	fclose(instream);

	smb_unlocksmbhdr(&smb);
	msg->hdr.length=(ushort)smb_getmsghdrlen(msg);
	if((i=smb_putmsghdr(&smb,msg))!=SMB_SUCCESS)
		errormsg(WHERE,ERR_WRITE,smb.file,i,smb.last_error);
}

/****************************************************************************/
/* Moves a message from one message base to another 						*/
/****************************************************************************/
bool sbbs_t::movemsg(smbmsg_t* msg, uint subnum)
{
	char str[256],*buf;
	uint i;
	int newgrp,newsub,storage;
	ulong offset,length;
	smbmsg_t	newmsg=*msg;
	smb_t		newsmb;

	for(i=0;i<usrgrps;i++)		 /* Select New Group */
		uselect(1,i,"Message Group",cfg.grp[usrgrp[i]]->lname,0);
	if((newgrp=uselect(0,0,0,0,0))<0)
		return(false);

	for(i=0;i<usrsubs[newgrp];i++)		 /* Select New Sub-Board */
		uselect(1,i,"Sub-Board",cfg.sub[usrsub[newgrp][i]]->lname,0);
	if((newsub=uselect(0,0,0,0,0))<0)
		return(false);
	newsub=usrsub[newgrp][newsub];

	length=smb_getmsgdatlen(msg);
	if((buf=(char *)malloc(length))==NULL) {
		errormsg(WHERE,ERR_ALLOC,smb.file,length);
		return(false); 
	}

	fseek(smb.sdt_fp,msg->hdr.offset,SEEK_SET);
	fread(buf,length,1,smb.sdt_fp);

	SAFEPRINTF2(newsmb.file,"%s%s",cfg.sub[newsub]->data_dir,cfg.sub[newsub]->code);
	newsmb.retry_time=cfg.smb_retry_time;
	newsmb.subnum=newsub;
	if((i=smb_open(&newsmb))!=SMB_SUCCESS) {
		free(buf);
		errormsg(WHERE,ERR_OPEN,newsmb.file,i,newsmb.last_error);
		return(false); 
	}

	if(filelength(fileno(newsmb.shd_fp))<1) {	 /* Create it if it doesn't exist */
		newsmb.status.max_crcs=cfg.sub[newsub]->maxcrcs;
		newsmb.status.max_msgs=cfg.sub[newsub]->maxmsgs;
		newsmb.status.max_age=cfg.sub[newsub]->maxage;
		newsmb.status.attr=cfg.sub[newsub]->misc&SUB_HYPER ? SMB_HYPERALLOC :0;
		if((i=smb_create(&newsmb))!=SMB_SUCCESS) {
			free(buf);
			smb_close(&newsmb);
			errormsg(WHERE,ERR_CREATE,newsmb.file,i,newsmb.last_error);
			return(false); 
		} 
	}

	if((i=smb_locksmbhdr(&newsmb))!=SMB_SUCCESS) {
		free(buf);
		smb_close(&newsmb);
		errormsg(WHERE,ERR_LOCK,newsmb.file,i,newsmb.last_error);
		return(false); 
	}

	if((i=smb_getstatus(&newsmb))!=SMB_SUCCESS) {
		free(buf);
		smb_close(&newsmb);
		errormsg(WHERE,ERR_READ,newsmb.file,i,newsmb.last_error);
		return(false); 
	}

	if(newsmb.status.attr&SMB_HYPERALLOC) {
		offset=smb_hallocdat(&newsmb);
		storage=SMB_HYPERALLOC; 
	}
	else {
		if((i=smb_open_da(&newsmb))!=SMB_SUCCESS) {
			free(buf);
			smb_close(&newsmb);
			errormsg(WHERE,ERR_OPEN,newsmb.file,i,newsmb.last_error);
			return(false); 
		}
		if(cfg.sub[newsub]->misc&SUB_FAST) {
			offset=smb_fallocdat(&newsmb,length,1);
			storage=SMB_FASTALLOC; 
		}
		else {
			offset=smb_allocdat(&newsmb,length,1);
			storage=SMB_SELFPACK; 
		}
		smb_close_da(&newsmb); 
	}

	newmsg.hdr.offset=offset;
	newmsg.hdr.version=smb_ver();

	fseek(newsmb.sdt_fp,offset,SEEK_SET);
	fwrite(buf,length,1,newsmb.sdt_fp);
	fflush(newsmb.sdt_fp);
	free(buf);

	i=smb_addmsghdr(&newsmb,&newmsg,storage);	// calls smb_unlocksmbhdr() 
	smb_close(&newsmb);

	if(i) {
		errormsg(WHERE,ERR_WRITE,newsmb.file,i,newsmb.last_error);
		smb_freemsg_dfields(&newsmb,&newmsg,1);
		return(false); 
	}

	bprintf("\r\nMoved to %s %s\r\n\r\n"
		,cfg.grp[usrgrp[newgrp]]->sname,cfg.sub[newsub]->lname);
	safe_snprintf(str,sizeof(str),"%s moved message from %s %s to %s %s"
		,useron.alias
		,cfg.grp[cfg.sub[subnum]->grp]->sname,cfg.sub[subnum]->sname
		,cfg.grp[newgrp]->sname,cfg.sub[newsub]->sname);
	logline("M+",str);
	signal_sub_sem(&cfg,newsub);

	return(true);
}

ushort sbbs_t::chmsgattr(ushort attr)
{
	int ch;

	while(online && !(sys_status&SS_ABORT)) {
		CRLF;
		show_msgattr(attr);
		menu("msgattr");
		ch=getkey(K_UPPER);
		if(ch)
			bprintf("%c\r\n",ch);
		switch(ch) {
			case 'P':
				attr^=MSG_PRIVATE;
				break;
			case 'R':
				attr^=MSG_READ;
				break;
			case 'K':
				attr^=MSG_KILLREAD;
				break;
			case 'A':
				attr^=MSG_ANONYMOUS;
				break;
			case 'N':   /* Non-purgeable */
				attr^=MSG_PERMANENT;
				break;
			case 'M':
				attr^=MSG_MODERATED;
				break;
			case 'V':
				attr^=MSG_VALIDATED;
				break;
			case 'D':
				attr^=MSG_DELETE;
				break;
			case 'L':
				attr^=MSG_LOCKED;
				break;
			default:
				return(attr); 
		} 
	}
	return(attr);
}
