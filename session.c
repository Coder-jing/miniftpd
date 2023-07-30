#include "common.h"
#include "session.h"
#include "ftpproto.h"
#include "privparent.h"
#include "privsock.h"
#include"sysutil.h"

void begin_session(session_t *sess)
{
	activate_oobinline(sess->ctrl_fd);
	//创建进程间通信通道
	priv_sock_init(sess);

	pid_t pid;
	pid = fork();
	if (pid < 0)
		ERR_EXIT("fork");

	if (pid == 0)
	{
		// ftp进程	处理客服端发送来的命令请求
		priv_sock_set_child_context(sess);
		handle_child(sess);
	}
	else
	{
		// nobody进程	协助建立与客户端的数据连接
		priv_sock_set_parent_context(sess);
		handle_parent(sess);
	}
}