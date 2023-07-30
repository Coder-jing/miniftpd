#ifndef _SESSION_H_
#define _SESSION_H_

#include "common.h"

typedef struct session
{
	// 控制连接
	uid_t uid;		/*登录用户ID*/
	int ctrl_fd;	/*控制连接文件描述符*/
	char cmdline[MAX_COMMAND_LINE];		/*原始命令行*/
	char cmd[MAX_COMMAND];				/*命令行*/
	char arg[MAX_ARG];				/*参数行*/

	// 数据连接
	struct sockaddr_in *port_addr;	//port模式客户端ip、端口
	int pasv_listen_fd;		//被动模式监听套接字
	int data_fd;	//数据套接字
	int data_process;	//上传指示符

	// 限速
	unsigned int bw_upload_rate_max;	//上传限速
	unsigned int bw_download_rate_max;	//下载限速
	long bw_transfer_start_sec;			//开始传输时间-秒
	long bw_transfer_start_usec;		//开始传输时间-微秒

	// 父子进程通信通道
	int parent_fd;		/*父亲端套接字*/
	int child_fd;		/*儿子端套接字*/

	// FTP协议状态
	int is_ascii;		//传输方式
	long long restart_pos;		//断点
	char *rnfr_name;		//重命名
	int abor_received;		//中断
	
	// 连接数限制
	unsigned int num_clients;
	unsigned int num_this_ip;
}  session_t;

void begin_session(session_t *sess);

#endif /* _SESSION_H_ */
