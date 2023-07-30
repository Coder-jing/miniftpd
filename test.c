#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

int main() {
    /* char ip[16] = {0};
    struct ifaddrs* ifaddr, *ifa;
	if(getifaddrs(&ifaddr) == -1)
	{
		return -1;
	}
	for(ifa = ifaddr;ifa != NULL;ifa = ifa->ifa_next)
	{
		if(ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name,"ens3")==0)
		{
            struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
			strcpy(ip,inet_ntoa(addr->sin_addr));
			//inet_ntop(AF_INET,&(addr->sin_addr),ip,INET_ADDRSTRLEN);
            break;
		}
        continue;
	}
    printf("%s\n",ip); */

	char hostname[16]; // NULL 表示当前主机
    if(gethostname(hostname,sizeof(hostname))<0)
    {
        perror("gethostname");
    }
    printf("hostname:%s\n",hostname);
    struct addrinfo hints, *result;

    // 设置 hints 结构体
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // 获取 IPv4 和 IPv6 地址
    hints.ai_socktype = SOCK_STREAM; // 套接字类型为流套接字
    hints.ai_flags = AI_PASSIVE;    // 获取所有本地地址

    // 获取主机信息
    if (getaddrinfo("localhost", NULL, &hints, &result) != 0) {
        perror("getaddrinfo");
        exit(1);
    }

    // 打印所有 IP 地址
    struct addrinfo* p;
    char ip[16];
    printf("All IP addresses of the host:\n");
    for (p = result; p != NULL; p = p->ai_next) {
       if(p->ai_family == AF_INET)
       {
            void *addr;
            if (p->ai_family == AF_INET) {
                struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
                addr = &(ipv4->sin_addr);
            }
            inet_ntop(p->ai_family, addr, ip, sizeof(ip));
            printf("- %s\n", ip);
       }
    }

    freeaddrinfo(result);

    return 0;
}