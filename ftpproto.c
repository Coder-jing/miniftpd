#include "ftpproto.h"
#include "sysutil.h"
#include "str.h"
#include "ftpcodes.h"
#include "tunable.h"
#include "privsock.h"
#include "common.h"

void ftp_lreply(session_t *sess, int status, const char *text);

int list_common(session_t *sess,int detail);

int get_port_fd(session_t *sess);
int get_transfer_fd(session_t *sess);
int port_active(session_t *sess);
int pasv_active(session_t *sess);

int get_pasv_fd(session_t *sess);
static void upload_common(session_t* sess,int is_appe);
void limit_rate(session_t* sess,int bytes_transfered,int is_upload);
long get_time_sec();
long get_time_usec();

void start_cmdio_alarm();
void start_data_alarm();

void handle_sigurg(int sig);
void check_abor(session_t* sess);

static void do_site_umask(session_t* sess,char* arg);
static void do_site_chmod(session_t* sess,char* arg);

static void do_user(session_t *sess);
static void do_pass(session_t *sess);
static void do_cwd(session_t *sess);
static void do_cdup(session_t *sess);
static void do_quit(session_t *sess);
static void do_port(session_t *sess);
static void do_pasv(session_t *sess);
static void do_type(session_t *sess);
//static void do_stru(session_t *sess);
//static void do_mode(session_t *sess);
static void do_retr(session_t *sess);
static void do_stor(session_t *sess);
static void do_appe(session_t *sess);
static void do_list(session_t *sess);
static void do_nlst(session_t *sess);
static void do_rest(session_t *sess);
static void do_abor(session_t *sess);
static void do_pwd(session_t *sess);
static void do_mkd(session_t *sess);
static void do_rmd(session_t *sess);
static void do_dele(session_t *sess);
static void do_rnfr(session_t *sess);
static void do_rnto(session_t *sess);
static void do_site(session_t *sess);
static void do_syst(session_t *sess);
static void do_feat(session_t *sess);
static void do_size(session_t *sess);
static void do_stat(session_t *sess);
static void do_noop(session_t *sess);
static void do_help(session_t *sess);

typedef struct ftpcmd
{
	const char *cmd;
	void (*cmd_handler)(session_t *sess);
} ftpcmd_t;


static ftpcmd_t ctrl_cmds[] = {
	/* 访问控制命令 */
	{"USER",	do_user	},
	{"PASS",	do_pass	},
	{"CWD",		do_cwd	},
	{"XCWD",	do_cwd	},
	{"CDUP",	do_cdup	},
	{"XCUP",	do_cdup	},
	{"QUIT",	do_quit	},
	{"ACCT",	NULL	},
	{"SMNT",	NULL	},
	{"REIN",	NULL	},
	/* 传输参数命令 */
	{"PORT",	do_port	},
	{"PASV",	do_pasv	},
	{"TYPE",	do_type	},
	{"STRU",	/*do_stru*/NULL	},
	{"MODE",	/*do_mode*/NULL	},

	/* 服务命令 */
	{"RETR",	do_retr	},
	{"STOR",	do_stor	},
	{"APPE",	do_appe	},
	{"LIST",	do_list	},
	{"NLST",	do_nlst	},
	{"REST",	do_rest	},
	{"ABOR",	do_abor	},
	{"\377\364\377\362ABOR", do_abor},
	{"PWD",		do_pwd	},
	{"XPWD",	do_pwd	},
	{"MKD",		do_mkd	},
	{"XMKD",	do_mkd	},
	{"RMD",		do_rmd	},
	{"XRMD",	do_rmd	},
	{"DELE",	do_dele	},
	{"RNFR",	do_rnfr	},
	{"RNTO",	do_rnto	},
	{"SITE",	do_site	},
	{"SYST",	do_syst	},
	{"FEAT",	do_feat },
	{"SIZE",	do_size	},
	{"STAT",	do_stat	},
	{"NOOP",	do_noop	},
	{"HELP",	do_help	},
	{"STOU",	NULL	},
	{"ALLO",	NULL	}
};

session_t* p_sess;

void handler_alarm_timeout(int sig)
{
	shutdown(p_sess->ctrl_fd,SHUT_RD);
	ftp_reply(p_sess,FTP_IDLE_TIMEOUT,"Timeout.");
	shutdown(p_sess->ctrl_fd,SHUT_WR);
	exit(EXIT_FAILURE);
}

void handler_sigalarm(int arg)
{
	if(!p_sess->data_process)
	{
		ftp_reply(p_sess,FTP_DATA_TIMEOUT,"Data timeout.Reconnect.Sorry.");
		exit(EXIT_FAILURE);
	}
	else	//处于数据传输状态
	{
		p_sess->data_process=0;
		start_data_alarm();
	}
}

void handle_sigurg(int sig)
{
	if(p_sess->data_fd==-1)
	{
		return;
	}
	char cmdline[MAX_COMMAND_LINE]={0};
	int ret=readline(p_sess->ctrl_fd,cmdline,MAX_COMMAND_LINE);
	if(ret<=0)
	{
		ERR_EXIT("redline");
	}
	str_trim_crlf(cmdline);
	if(strcpy(cmdline,"ABOR")==0||strcpy(cmdline,"\377\364\377\362ABOR")==0)
	{
		p_sess->abor_received=1;
		shutdown(p_sess->data_fd,SHUT_RDWR);
	}
	else
	{
		ftp_reply(p_sess,FTP_BADCMD,"Unknown command.");
	}
}

void check_abor(session_t* sess)
{
	if(sess->abor_received)
	{
		sess->abor_received=0;
		ftp_reply(sess,FTP_ABOROK,"ABOR successful.");
	}
}

void start_cmdio_alarm()
{
	if(tunable_idle_session_timeout>0)
	{
		//安装信号
		signal(SIGALRM,handler_alarm_timeout);
		//启动闹钟
		alarm(tunable_idle_session_timeout);
	}
}

void start_data_alarm()
{
	if(tunable_data_connection_timeout>0)
	{
		//安装信号
		signal(SIGALRM,handler_sigalarm);
		//启动闹钟
		alarm(tunable_data_connection_timeout);
	}
	else if(tunable_idle_session_timeout>0)
	{
		//关闭先前安装的闹钟
		alarm(0);
	}
}

void handle_child(session_t *sess)
{
	ftp_reply(sess, FTP_GREET, "(miniftpd 0.1)");
	int ret;
	while (1)
	{
		memset(sess->cmdline, 0, sizeof(sess->cmdline));
		memset(sess->cmd, 0, sizeof(sess->cmd));
		memset(sess->arg, 0, sizeof(sess->arg));
		//启动闲置自动断开闹钟
		start_cmdio_alarm();
		
		ret = readline(sess->ctrl_fd, sess->cmdline, MAX_COMMAND_LINE);
		if (ret == -1)
			ERR_EXIT("readline");
		else if (ret == 0)
			exit(EXIT_SUCCESS);

		printf("cmdline=[%s]\n", sess->cmdline);
		//去除\r\n
		str_trim_crlf(sess->cmdline);
		printf("cmdline=[%s]\n", sess->cmdline);
		//按照空格分开字符串
		str_split(sess->cmdline, sess->cmd, sess->arg, ' ');
		printf("cmd=[%s] arg=[%s]\n", sess->cmd, sess->arg);
		//将命令转换为大写
		str_upper(sess->cmd);
		//与命令表进行比对
		int i;
		int size = sizeof(ctrl_cmds) / sizeof(ctrl_cmds[0]);
		for (i=0; i<size; i++)
		{
			if (strcmp(ctrl_cmds[i].cmd, sess->cmd) == 0)
			{
				if (ctrl_cmds[i].cmd_handler != NULL)
				{
					ctrl_cmds[i].cmd_handler(sess);
				}
				else
				{
					ftp_reply(sess, FTP_COMMANDNOTIMPL, "Unimplement command.");
				}
				
				break;
			}
		}

		if (i == size)
		{
			ftp_reply(sess, FTP_BADCMD, "Unknown command.");
		}
	}
}

void ftp_reply(session_t *sess, int status, const char *text)
{
	char buf[1024] = {0};
	sprintf(buf, "%d %s\r\n", status, text);
	writen(sess->ctrl_fd, buf, strlen(buf));
}

void ftp_lreply(session_t *sess, int status, const char *text)
{
	char buf[1024] = {0};
	sprintf(buf, "%d-%s\r\n", status, text);
	writen(sess->ctrl_fd, buf, strlen(buf));
}

int list_common(session_t *sess,int detail)
{
	DIR *dir = opendir(".");
	if (dir == NULL)
	{
		return 0;
	}

	struct dirent *dt;
	struct stat sbuf;
	while ((dt = readdir(dir)) != NULL)
	{
		
		if (lstat(dt->d_name, &sbuf) < 0)
		{
			continue;
		}
		if (dt->d_name[0] == '.')
			continue;
		
		char buf[1024] = {0};
		if(detail)
		{
			const char* perms=statbuf_get_perms(&sbuf);
			/* -rw-rw-r--  1 tiger_jy tiger_jy    278 Mar  5 09:41 Makefile */
			int off = 0;
			off += sprintf(buf, "%s ", perms);
			off += sprintf(buf + off, " %3ld %-8d %-8d ", sbuf.st_nlink, sbuf.st_uid, sbuf.st_gid);
			off += sprintf(buf + off, "%8lu ", (unsigned long)sbuf.st_size);

			const char *datebuf = statbuf_get_date(&sbuf);
			off += sprintf(buf + off, "%s ", datebuf);
			if (S_ISLNK(sbuf.st_mode))
			{
				char tmp[512] = {0};
				readlink(dt->d_name, tmp, sizeof(tmp));
				off += sprintf(buf + off, "%s -> %s\r\n", dt->d_name, tmp);
			}
			else
			{
				off += sprintf(buf + off, "%s\r\n", dt->d_name);
			}
		}
		else
		{
			sprintf(buf, "%s\r\n", dt->d_name);
		}
		
		writen(sess->data_fd, buf, strlen(buf));
	}

	closedir(dir);

	return 1;
}

int port_active(session_t *sess)
{
	if (sess->port_addr)
	{
		if (pasv_active(sess))
		{
			fprintf(stderr, "both port and pasv are active");
			exit(EXIT_FAILURE);
		}
		return 1;
	}

	return 0;
}

int pasv_active(session_t *sess)
{
	priv_sock_send_cmd(sess->child_fd,PRIV_SOCK_PASV_ACTIVE);
	int active=priv_sock_get_int(sess->child_fd);
	if(active)
	{
		if (port_active(sess))
		{
			fprintf(stderr, "both port an pasv are active");
			exit(EXIT_FAILURE);
		}
		return 1;
	}
	return 0;
}

int get_port_fd(session_t *sess)
{
	/*
		向nobody发送PRIV_SOCK_GET_DATA_SOCK命令        1
		向nobody发送一个整数port		       4
		向nobody发送一个字符串ip                       不定长
	*/

	priv_sock_send_cmd(sess->child_fd, PRIV_SOCK_GET_DATA_SOCK);
	unsigned short port = ntohs(sess->port_addr->sin_port);
	
	char *ip = inet_ntoa(sess->port_addr->sin_addr);
	
	priv_sock_send_int(sess->child_fd, (int)port);
	priv_sock_send_buf(sess->child_fd, ip, strlen(ip));
	
	char res = priv_sock_get_result(sess->child_fd);

	if (res == PRIV_SOCK_RESULT_BAD)
	{
		return 0;
	}
	else if (res == PRIV_SOCK_RESULT_OK)
	{
		sess->data_fd = priv_sock_recv_fd(sess->child_fd);
	}

	return 1;
}

int get_pasv_fd(session_t *sess)
{
	priv_sock_send_cmd(sess->child_fd, PRIV_SOCK_PASV_ACCEPT);
	char res = priv_sock_get_result(sess->child_fd);
	if (res == PRIV_SOCK_RESULT_BAD)
	{
		return 0;
	}
	else if (res == PRIV_SOCK_RESULT_OK)
	{
		sess->data_fd = priv_sock_recv_fd(sess->child_fd);
	}

	return 1;
}

int get_transfer_fd(session_t *sess)
{
	// 检测是否收到PORT或者PASV命令
	if (!port_active(sess) && !pasv_active(sess))
	{
		ftp_reply(sess, FTP_BADSENDCONN, "Use PORT or PASV first.");
		return 0;
	}

	int ret = 1;
	// 如果是主动模式
	if (port_active(sess))
	{
		if (get_port_fd(sess) == 0)
		{
			ret = 0;
		}
	}

	if (pasv_active(sess))
	{
		if (get_pasv_fd(sess) == 0)
		{
			ret = 0;
		}

	}

	if (sess->port_addr)
	{
		free(sess->port_addr);
		sess->port_addr = NULL;
	}

	if(ret)
	{
		//重新安装信号并启动闹钟
		start_data_alarm();
	}

	return ret;
}

static void do_user(session_t *sess)
{
	//USER tiger_jy
	struct passwd *pw = getpwnam(sess->arg);
	if (pw == NULL)
	{
		// 用户不存在
		ftp_reply(sess, FTP_LOGINERR, "Login incorrect.");
		return;
	}

	sess->uid = pw->pw_uid;
	ftp_reply(sess, FTP_GIVEPWORD, "Please specify the password.");
	
}

static void do_pass(session_t *sess)
{
	// PASS 123456
	struct passwd *pw = getpwuid(sess->uid);
	if (pw == NULL)
	{
		// 用户不存在
		ftp_reply(sess, FTP_LOGINERR, "Login incorrect.");
		return;
	}

	struct spwd *sp = getspnam(pw->pw_name);
	if (sp == NULL)
	{
		ftp_reply(sess, FTP_LOGINERR, "Login incorrect.");
		return;
	}

	// 将明文加密
	char *encrypted_pass = crypt(sess->arg, sp->sp_pwdp);
	// 验证密码
	if (strcmp(encrypted_pass, sp->sp_pwdp) != 0)
	{
		ftp_reply(sess, FTP_LOGINERR, "Login incorrect.");
		return;
	}

	signal(SIGURG,handle_sigurg);
	activate_sigurg(sess->ctrl_fd);

	umask(tunable_local_umask);
	setegid(pw->pw_gid);
	seteuid(pw->pw_uid);
	chdir(pw->pw_dir);
	ftp_reply(sess, FTP_LOGINOK, "Login successful.");
}

static void do_cwd(session_t *sess)
{
	if(chdir(sess->arg)<0)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Fail to change directory.");
		return;
	}
	ftp_reply(sess,FTP_CWDOK,"Directory successfully changed.");
}

static void do_cdup(session_t *sess)
{
	if(chdir("..")<0)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Fail to change directory.");
		return;
	}
	ftp_reply(sess,FTP_CWDOK,"Directory successfully changed.");
}

static void do_quit(session_t *sess)
{
	ftp_reply(sess,FTP_GOODBYE,"Goodby.");
	exit(EXIT_SUCCESS);
}

static void do_port(session_t *sess)
{
	//PORT 192,168,3,20,228,111
	unsigned int v[6];

	sscanf(sess->arg, "%u,%u,%u,%u,%u,%u", &v[2], &v[3], &v[4], &v[5], &v[0], &v[1]);
	sess->port_addr = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in));
	memset(sess->port_addr, 0, sizeof(struct sockaddr_in));
	sess->port_addr->sin_family = AF_INET;
	unsigned char *p = (unsigned char *)&sess->port_addr->sin_port;
	p[0] = v[0];
	p[1] = v[1];

	p = (unsigned char *)&sess->port_addr->sin_addr;
	p[0] = v[2];
	p[1] = v[3];
	p[2] = v[4];
	p[3] = v[5];

	ftp_reply(sess, FTP_PORTOK, "PORT command successful. Consider using PASV.");
}

static void do_pasv(session_t *sess)
{
	//227 Entering Passive Mode (192,168,3,9,171,187).

	char ip[16] = {0};
	getlocalip(ip);
	priv_sock_send_cmd(sess->child_fd, PRIV_SOCK_PASV_LISTEN);
	unsigned short port = (int)priv_sock_get_int(sess->child_fd);


	unsigned int v[4];
	sscanf(ip, "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]);
	char text[1024] = {0};
	sprintf(text, "Entering Passive Mode (%u,%u,%u,%u,%u,%u).", 
		v[0], v[1], v[2], v[3], port>>8, port&0xFF);

	ftp_reply(sess, FTP_PASVOK, text);
}

static void do_type(session_t *sess)
{
	if (strcmp(sess->arg, "A") == 0)
	{
		sess->is_ascii = 1;
		ftp_reply(sess, FTP_TYPEOK, "Switching to ASCII mode.");
	}
	else if (strcmp(sess->arg, "I") == 0)
	{
		sess->is_ascii = 0;
		ftp_reply(sess, FTP_TYPEOK, "Switching to Binary mode.");
	}
	else
	{
		ftp_reply(sess, FTP_BADCMD, "Unrecognised TYPE command.");
	}

}

// static void do_stru(session_t *sess)
// {
// }

// static void do_mode(session_t *sess)
// {
// }

static void do_retr(session_t *sess)
{
	//创建数据连接
	if (get_transfer_fd(sess) == 0)
	{
		printf("in get_transfer_fd()\n");
		return;
	}
	//设置断点
	long long offset = sess->restart_pos;
	sess->restart_pos = 0;
	//打开文件
	int fd=open(sess->arg,O_RDONLY);
	if(fd==-1)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Failed to open file.");
		return;
	}
	//加读锁
	int  ret;
	ret=lock_file_read(fd);
	if(ret==-1)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Failed to open file.");
		return;
	}
	//判断是否是普通文件
	struct stat st;
	ret=fstat(fd,&st);
	if(!S_ISREG(st.st_mode))
	{
		ftp_reply(sess,FTP_FILEFAIL,"Failed to open file.");
		return;
	}
	//判断是否有断点
	if (offset != 0)
	{
		ret = lseek(fd, offset, SEEK_SET);
		if (ret == -1)
		{
			ftp_reply(sess, FTP_FILEFAIL, "Failed to open file.");
			return;
		}
	}
	// 150
	char text[2048]={0};
	if(sess->is_ascii)
	{
		sprintf(text,"Opening ASCII mode data connection for %s (%lld bytes).",sess->arg,(long long)st.st_size);
	}
	else
	{
		sprintf(text,"Opening BINARY mode data connection for %s (%lld bytes).",sess->arg,(long long)st.st_size);
	}
	ftp_reply(sess, FTP_DATACONN, text);

	// 下载文件
	int flag=0;
	long long bytes_to_send = st.st_size;
	if(offset>bytes_to_send)
	{
		bytes_to_send=0;
	}
	else
	{
		bytes_to_send-=offset;
	}
	sess->bw_transfer_start_sec=get_time_sec();
	sess->bw_transfer_start_usec=get_time_usec();
	while(bytes_to_send)
	{
		int num_send_this=bytes_to_send>4096?4096:bytes_to_send;
		ret=sendfile(sess->data_fd,fd,NULL,num_send_this);
		if(ret<0)
		{
			flag=2;
			break;
		}
		limit_rate(sess,ret,0);

		if(sess->abor_received)
		{
			flag=2;
			break;
		}

		bytes_to_send-=ret;
	}
	if(bytes_to_send==0)
	{
		flag=0;
	}
	// 关闭数据套接字
	close(sess->data_fd);
	sess->data_fd=-1;
	close(fd);
	if(flag==0)
	{
		ftp_reply(sess,FTP_TRANSFEROK,"Transfer complete.");
	}
	else if(flag==1)
	{
		ftp_reply(sess,FTP_BADSENDFILE,"Failed reading from local file.");
	}
	else
	{
		ftp_reply(sess,FTP_BADSENDNET,"Failure writing to network stream.");
	}
	check_abor(sess);
	//重新开启控制连接通道闹钟
	start_cmdio_alarm();
}

void limit_rate(session_t* sess,int bytes_transfered,int is_upload)
{
	p_sess->data_process=1;
	//睡眠时间=（当前传输速度/最大传输速度-1)*当前传输时间
	long cur_sec=get_time_sec();
	long cur_usec=get_time_usec();

	double elapsed;
	elapsed=cur_sec-sess->bw_transfer_start_sec;
	elapsed+=(double)(cur_usec-sess->bw_transfer_start_usec)/(double)1000000;
	if(elapsed<=0)
	{
		elapsed=0.01;
	}
	//计算传输速度
	unsigned int bw_rate=(unsigned int)((double)bytes_transfered/elapsed);
	double rate_ratio;
	if(is_upload)
	{
		if(bw_rate<sess->bw_upload_rate_max)
		{
			sess->bw_transfer_start_sec=get_time_sec();
			sess->bw_transfer_start_usec=get_time_usec();
			return;
		}
		rate_ratio=bw_rate/sess->bw_upload_rate_max;
	}
	else
	{
		if(bw_rate<sess->bw_download_rate_max)
		{
			sess->bw_transfer_start_sec=get_time_sec();
			sess->bw_transfer_start_usec=get_time_usec();
			return;
		}
		rate_ratio=bw_rate/sess->bw_download_rate_max;
	}
	double pause_time;
	pause_time=(rate_ratio-(double)1)*elapsed;
	nano_sleep(pause_time);
	sess->bw_transfer_start_sec=get_time_sec();
	sess->bw_transfer_start_usec=get_time_usec();
}

static void upload_common(session_t* sess,int is_append)
{
	//创建数据连接
	if (get_transfer_fd(sess) == 0)
	{
		return;
	}
	//设置断点
	long long offset = sess->restart_pos;
	sess->restart_pos = 0;
	//打开文件
	int fd=open(sess->arg,O_CREAT|O_WRONLY,0666);
	if(fd==-1)
	{
		ftp_reply(sess,FTP_UPLOADFAIL,"Could not create file.");
		return;
	}
	//加写锁
	int  ret;
	ret=lock_file_write(fd);
	if(ret==-1)
	{
		ftp_reply(sess,FTP_UPLOADFAIL,"Could not create file.");
		return;
	}
	//模式	STOR | REST+STOR | APPE
	if(!is_append&&offset==0)	//STOR
	{
		ftruncate(fd,0);	//清零
		if(lseek(fd,0,SEEK_SET)<0)
		{
			ftp_reply(sess,FTP_UPLOADFAIL,"Could not create file.");
			return;
		}
	}
	else if(!is_append&&offset!=0)	//REST+STOR
	{
		if(lseek(fd,offset,SEEK_SET)<0)
		{
			ftp_reply(sess,FTP_UPLOADFAIL,"Could not create file.");
			return;
		}
	}
	else if(is_append)				//APPE
	{
		if(lseek(fd,0,SEEK_END)<0)
		{
			ftp_reply(sess,FTP_UPLOADFAIL,"Could not create file.");
			return;
		}
	}
	//判断是否有断点
	if (offset != 0)
	{
		ret = lseek(fd, offset, SEEK_SET);
		if (ret == -1)
		{
			ftp_reply(sess, FTP_FILEFAIL, "Failed to open file.");
			return ;
		}
	}
	// 150
	char text[2048]={0};

	struct stat st;
	ret=fstat(fd,&st);
	if(sess->is_ascii)
	{
		sprintf(text,"Opening ASCII mode data connection for %s (%lld bytes).",sess->arg,(long long)st.st_size);
	}
	else
	{
		sprintf(text,"Opening BINARY mode data connection for %s (%lld bytes).",sess->arg,(long long)st.st_size);
	}
	ftp_reply(sess, FTP_DATACONN, text);
	//睡眠时间=（当前传输速度/最大传输速度-1)*当前传输时间
	sess->bw_transfer_start_sec=get_time_sec();
	sess->bw_transfer_start_usec=get_time_usec();
	// 上传文件
	char buf[4096]={0};
	int flag=0;

	while(1)
	{
		int rd=read(sess->data_fd,buf,sizeof(buf));
		if(rd==-1)
		{
			if(errno==EINTR)
			{
				continue;
			}
			else
			{
				flag=2;
				break;
			}
		}
		else if(rd==0)
		{
			flag=0;
			break;
		}

		limit_rate(sess,rd,1);

		if(sess->abor_received)
		{
			flag=2;
			break;
		}

		if(writen(fd,buf,rd)!=rd)
		{
			flag=1;
			break;
		}
	}
	// 关闭数据套接字
	close(sess->data_fd);
	sess->data_fd=-1;
	//关闭文件
	close(fd);
	if(flag==0&&!sess->abor_received)
	{
		ftp_reply(sess,FTP_TRANSFEROK,"Transfer complete.");
	}
	else if(flag==1)
	{
		ftp_reply(sess,FTP_BADSENDFILE,"Failed writting to local file.");
	}
	else
	{
		ftp_reply(sess,FTP_BADSENDNET,"Failure reading from network stream.");
	}
	//检测是否有ABOR命令过来
	check_abor(sess);
	//重新开启控制连接通道闹钟
	start_cmdio_alarm();
}

static void do_stor(session_t *sess)
{
	upload_common(sess,0);
}

static void do_appe(session_t *sess)
{
	upload_common(sess,1);
}

static void do_list(session_t *sess)
{
	// 创建数据连接
	if (get_transfer_fd(sess) == 0)
	{
		return;
	}
	// 150
	ftp_reply(sess, FTP_DATACONN, "Here comes the directory listing.");

	// 传输列表
	list_common(sess, 1);
	// 关闭数据套接字
	close(sess->data_fd);
	sess->data_fd = -1;
	// 226
	ftp_reply(sess, FTP_TRANSFEROK, "Directory send OK.");

}

static void do_nlst(session_t *sess)
{
	// 创建数据连接
	if (get_transfer_fd(sess) == 0)
	{
		return;
	}
	// 150
	ftp_reply(sess, FTP_DATACONN, "Here comes the directory listing.");

	// 传输列表
	list_common(sess, 0);
	// 关闭数据套接字
	close(sess->data_fd);
	sess->data_fd = -1;
	// 226
	ftp_reply(sess, FTP_TRANSFEROK, "Directory send OK.");
}

static void do_rest(session_t *sess)
{
	sess->restart_pos=str_to_longlong(sess->arg);	//atoll()
	char text[1024]={0};
	sprintf(text,"Restart position accepted (%lld).",sess->restart_pos);
	ftp_reply(sess,FTP_RESTOK,text);
}

static void do_abor(session_t *sess)
{
	ftp_reply(sess,FTP_ABOR_NOCONN,"No transfer to ABOR");
}

static void do_pwd(session_t *sess)
{
	char text[1024] = {0};
	char dir[512] = {0};
	getcwd(dir, 1024);
	sprintf(text, "\"%s\"", dir);

	ftp_reply(sess, FTP_PWDOK, text);
}

static void do_mkd(session_t *sess)
{
	if(mkdir(sess->arg,0777)<0)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Create directory operation failed.");
		return;
	}
	char text[2048]={0};
	if(sess->arg[0]=='/')
	{
		sprintf(text,"%s created",sess->arg);
	}
	else
	{
		//相对路径
		char buf[512]={0};
		getcwd(buf,512);
		if(buf[strlen(buf)-1]!='/')
		{
			sprintf(text,"%s/%s created",buf,sess->arg);
		}
		else
		{
			sprintf(text,"%s%s created",buf,sess->arg);
		}
	}
	ftp_reply(sess,FTP_MKDIROK,"Directory send OK.");
}

static void do_rmd(session_t *sess)
{
	if(rmdir(sess->arg)<0)
	{
		ftp_reply(sess,FTP_DELEOK,"Remove directory failed.");
		return;
	}
	ftp_reply(sess,FTP_RMDIROK,"Remove directory successfully.");
}

static void do_dele(session_t *sess)
{
	if(unlink(sess->arg)<0)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Delete operation failed.");
		return;
	}
	ftp_reply(sess,FTP_DELEOK,"Delete operation successfully.");
}

static void do_rnfr(session_t *sess)
{
	sess->rnfr_name=malloc(strlen(sess->arg)+1);
	memset(sess->rnfr_name,0,sizeof(strlen(sess->arg)+1));
	strcpy(sess->rnfr_name,sess->arg);
	ftp_reply(sess,FTP_RNFROK,"Ready for RNTO.");
}

static void do_rnto(session_t *sess)
{
	if(!sess->rnfr_name)
	{
		ftp_reply(sess,FTP_NEEDRNFR,"RNFR required first.");
		return;
	}
	else
	{
		rename(sess->rnfr_name,sess->arg);
		ftp_reply(sess,FTP_RENAMEOK,"Rename successfully.");
		free(sess->rnfr_name);
		sess->rnfr_name=NULL;
	}
}

static void do_site_umask(session_t* sess,char* arg)
{
	if(strlen(arg)==0)
	{
		char text[1024]={0};
		sprintf(text,"Your current umask is 0%o",tunable_local_umask);
		ftp_reply(sess,FTP_UMASKOK,text);
	}
	else
	{
		unsigned int umask_arg=str_octal_to_uint(arg);
		umask(umask_arg);
		char text[1024]={0};
		sprintf(text,"UMASK set to 0%o",umask_arg);
		ftp_reply(sess,FTP_UMASKOK,text);
	}
}

static void do_site_chmod(session_t* sess,char* arg)
{
	//SITE CHMOD <perm> <file>
	if(strlen(arg)==0)
	{
		ftp_reply(sess,FTP_BADCMD,"SITE CHMOD needs 2 arguments.");
		return;
	}
	char perm[128]={0};
	char file[128]={0};
	str_split(arg,perm,file,' ');
	if(strlen(file)==0)
	{
		ftp_reply(sess,FTP_BADCMD,"SITE CHMOD needs 2 arguments.");
		return;
	}
	unsigned int mode=str_octal_to_uint(perm);
	if(chmod(file,mode)<0)
	{
		ftp_reply(sess,FTP_CHMODOK,"SITE CHMOD command failed.");
		return;
	}
	ftp_reply(sess,FTP_CHMODOK,"SITE CHMOD command OK.");
}

static void do_site(session_t *sess)
{
	char cmd[100]={0};
	char arg[100]={0};

	str_split(sess->arg,cmd,arg,' ');
	if(strcmp(cmd,"CHMOD")==0)
	{
		do_site_chmod(sess,arg);
	}
	else if(strcmp(cmd,"UMASK")==0)
	{
		do_site_umask(sess,arg);
	}
	else if(strcmp(cmd,"HELP")==0)
	{
		ftp_reply(sess,FTP_SITEHELP,"CHMOD UMASK HELP");
	}
	else
	{
		ftp_reply(sess,FTP_BADCMD,"Unknown SITE command.");
	}
}

static void do_syst(session_t *sess)
{
	ftp_reply(sess, FTP_SYSTOK, "UNIX Type: L8");
}

static void do_feat(session_t *sess)
{
	ftp_lreply(sess, FTP_FEAT, "Features:");
	writen(sess->ctrl_fd, " EPRT\r\n", strlen(" EPRT\r\n"));
	writen(sess->ctrl_fd, " EPSV\r\n", strlen(" EPSV\r\n"));
	writen(sess->ctrl_fd, " MDTM\r\n", strlen(" MDTM\r\n"));
	writen(sess->ctrl_fd, " PASV\r\n", strlen(" PASV\r\n"));
	writen(sess->ctrl_fd, " REST STREAM\r\n", strlen(" REST STREAM\r\n"));
	writen(sess->ctrl_fd, " SIZE\r\n", strlen(" SIZE\r\n"));
	writen(sess->ctrl_fd, " TVFS\r\n", strlen(" TVFS\r\n"));
	writen(sess->ctrl_fd, " UTF8\r\n", strlen(" UTF8\r\n"));
	ftp_reply(sess, FTP_FEAT, "End");
}

static void do_size(session_t *sess)
{
	struct stat st;
	if(stat(sess->arg,&st)<0)
	{
		ftp_reply(sess,FTP_FILEFAIL,"SIZE operation failed.");
		return;
	}
	if(S_ISREG(st.st_mode))
	{
		char text[1024]={0};
		sprintf(text,"%ld",st.st_size);
		ftp_reply(sess,FTP_SIZEOK,text);
	}
	else
	{
		ftp_reply(sess,FTP_BADMODE,"Could not get file size.");
	}
}

static void do_stat(session_t *sess)
{
	ftp_lreply(sess, FTP_STATOK, "FTP server status:");
	if (sess->bw_upload_rate_max == 0)
	{
		char text[1024];
		sprintf(text,
			"     No session upload bandwidth limit\r\n");
		writen(sess->ctrl_fd, text, strlen(text));
	}
	else if (sess->bw_upload_rate_max > 0)
	{
		char text[1024];
		sprintf(text,
			"     Session upload bandwidth limit in byte/s is %u\r\n",
			sess->bw_upload_rate_max);
		writen(sess->ctrl_fd, text, strlen(text));
	}

	if (sess->bw_download_rate_max == 0)
	{
		char text[1024];
		sprintf(text,
			"     No session download bandwidth limit\r\n");
		writen(sess->ctrl_fd, text, strlen(text));
	}
	else if (sess->bw_download_rate_max > 0)
	{
		char text[1024];
		sprintf(text,
			"     Session download bandwidth limit in byte/s is %u\r\n",
			sess->bw_download_rate_max);
		writen(sess->ctrl_fd, text, strlen(text));
	}

	char text[1024] = {0};
	sprintf(text,
		"     At session startup, client count was %u\r\n",
		sess->num_clients);
	writen(sess->ctrl_fd, text, strlen(text));
	
	ftp_reply(sess, FTP_STATOK, "End of status");
}

static void do_noop(session_t *sess)
{
	ftp_reply(sess,FTP_NOOPOK,"NOOP ok.");
}

static void do_help(session_t *sess)
{
	ftp_lreply(sess, FTP_HELP, "The following commands are recognized.");
	writen(sess->ctrl_fd,
		" ABOR ACCT ALLO APPE CDUP CWD  DELE EPRT EPSV FEAT HELP LIST MDTM MKD\r\n",
		strlen(" ABOR ACCT ALLO APPE CDUP CWD  DELE EPRT EPSV FEAT HELP LIST MDTM MKD\r\n"));
	writen(sess->ctrl_fd,
		" MODE NLST NOOP OPTS PASS PASV PORT PWD  QUIT REIN REST RETR RMD  RNFR\r\n",
		strlen(" MODE NLST NOOP OPTS PASS PASV PORT PWD  QUIT REIN REST RETR RMD  RNFR\r\n"));
	writen(sess->ctrl_fd,
		" RNTO SITE SIZE SMNT STAT STOR STOU STRU SYST TYPE USER XCUP XCWD XMKD\r\n",
		strlen(" RNTO SITE SIZE SMNT STAT STOR STOU STRU SYST TYPE USER XCUP XCWD XMKD\r\n"));
	writen(sess->ctrl_fd,
		" XPWD XRMD\r\n",
		strlen(" XPWD XRMD\r\n"));
	ftp_reply(sess, FTP_HELP, "Help OK.");
}

