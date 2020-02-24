//ATM服务器端
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/shm.h>

#define MSG_SERVER_FILE "./msg_server"	//用于创建server端发送、client端接收的消息队列所需文件
#define MSG_CLIENT_FILE "./msg_client"	//用于创建server端接收、client端发送的消息队列所需文件
#define MSG_SHMGET_FILE "./msg_shmget"	//用于创建共享内存所需文件

//服务器端开户信息
struct AccountInfo
{
	char name[20];
	char sex;
	int age;
	long id;	//身份证号
	unsigned char passwd[10];	//6位密码
	unsigned int cardNum;	//卡号
	unsigned int money;
};

struct Message
{
	char mtext[1024];
	struct AccountInfo account;
};

//服务器端发送数据结构体定义
typedef struct serverMsg
{
	long mtype;
	struct Message data;
}serverMsg_t;

//菜单数据
char MenuBuf[] = {"\t1 - 菜单\n\t2 - 开户\n\t3 - 登录账户\n\t4 - 存款\n\t5 - 取款\n\t6 - 查询余额\n\t7 - 转账\n\t8 - 退出登录\n\t9 - 销户\n"};

//登录信息结构体
struct LoginInfo
{
	int flag; //登录状态（0：未登录 1：已登录）
	unsigned int cardNum;	//登录账号，即卡号，也即文件名
};

int flag_exit = 0;  //程序退出标志（1为退出）


void recycle_exit(int signum)
{
	while( waitpid(-1, NULL, WNOHANG) > 0 );

	flag_exit = 1;
}


int main(int argc, char *argv[])
{
	int fd_shm = open(MSG_SHMGET_FILE, O_RDWR | O_CREAT, 0666);	//创建获取key值所需文件
	if(fd_shm == -1)
	{
		perror("open error");
		return -1;
	}
	int key_shm = ftok(MSG_SHMGET_FILE, 255);	//获取key值
	if(key_shm == -1)
	{
		perror("ftok error");
		return -1;
	}
	int shmid = shmget(key_shm, sizeof(struct LoginInfo), IPC_CREAT | 0666);	
	if(shmid == -1)
	{
		perror("shmget error");
		return -1;
	}
	struct LoginInfo *login;	//定义登录信息结构体变量指针
	login = (struct LoginInfo*)shmat(shmid, NULL, 0);	
	if(login == NULL)
	{
		perror("shmat error");
		return -1;
	}
	login->flag = 0;	//登录状态初始化为：未登录



	//创建两个消息队列
	int fd_s = open(MSG_SERVER_FILE, O_RDWR | O_CREAT, 0666);	//创建获取key值所需文件
	int fd_c = open(MSG_CLIENT_FILE, O_RDWR | O_CREAT, 0666);	
	if((fd_s == -1) || (fd_c == -1))
	{
		perror("open error");
		return -1;
	}

	int key_s = ftok(MSG_SERVER_FILE, 255);	//获取key值
	int key_c = ftok(MSG_CLIENT_FILE, 255);	
	if((key_s == -1) || (key_c == -1))
	{
		perror("ftok error");
		return -1;
	}

	int msgid_server = msgget(key_s, IPC_CREAT | IPC_EXCL | 0664);	//创建消息队列：server发送，client接收
	int msgid_client = msgget(key_c, IPC_CREAT | IPC_EXCL | 0664);	//创建消息队列：client发送，server接收
	if((msgid_server == -1) || (msgid_client == -1))
	{
		perror("msgget error");
		return -1;
	}

	serverMsg_t readMsg = {0, {0}};	//定义读取消息队列缓存
	serverMsg_t writeMsg = {0, {0}};	//定义发送消息队列缓存

	struct sigaction sig_usr1;
	sig_usr1.sa_handler = recycle_exit;
	sig_usr1.sa_flags = 0;
	sigemptyset(&sig_usr1.sa_mask);
	sigaction(SIGUSR1, &sig_usr1, NULL);


	while(1)
	{
		int msg_len = msgrcv(msgid_client, &readMsg, sizeof(readMsg.data), 0, 0);	//父进程读取消息队列
		if(msg_len == -1)
		{
			if(flag_exit == 1)  //即将退出
			{
				sleep(3);	//等待客户端先退出
				printf("服务器端已退出\n");
				goto process_exit;
			}

			perror("msgrcv parretn error");
			return -1;
		}

		printf("receive from client\n");

		pid_t pid = fork();	//父进程收到一条信息，则创建一个子进程
		if(pid == 0)	//子进程将消息分类，并进行相应处理
		{
			//首先读取当前的登录状态
			printf("当前登录状态：%d\n", login->flag);
			switch (readMsg.mtype)
			{
				case 1:	//向消息队列写入菜单数据
					writeMsg.mtype = 1;	//发送菜单项
					strncpy(writeMsg.data.mtext, MenuBuf, strlen(MenuBuf));
					msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data.mtext), 0);
					break;
				case 2:	//回应客户端提出的开户申请（创建账户文件、向客户端发送开户成功后的信息）
					writeMsg.mtype = 2;	//设置发送消息类型
					if(login->flag == 1)	//如果已登录，则不允许开户
					{
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "当前已登录，请退出当前登录再进行开户操作！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}

					srand(time(NULL));	
					unsigned int rand_num = rand()%1000;	//生成随机卡号
					//判断客户提交ID是否与已有账户的ID重复，若重复则开户失败（即每个人只能开一个账户）
					DIR *dir_account = opendir("./Account");	//打开账户文件储存目录
					if(dir_account == NULL)
					{
						perror("opendir error");
						return -1;
					}
					struct dirent *dirp = NULL;
					char id_buf[1024] = {0};
					while((dirp = readdir(dir_account)) != NULL)	//遍历账户文件目录
					{
						if(dirp->d_type == DT_REG)	//如果是普通文件，则读取文件的ID号
						{
							char filename[256] = {0};
							sprintf(filename, "./Account/%s", dirp->d_name);
							FILE *fp = fopen(filename, "r");
							if(fp == NULL)
							{
								perror("fopen error");
								return -1;
							}
							for(int i = 0; i < 4; ++i)	//按行读取4次，最后一次即读到ID
							{
								memset(id_buf, 0, sizeof(id_buf));
								fgets(id_buf, sizeof(id_buf), fp);
							}
							//printf("id: %s\n", id_buf);
							long id_src = atol(id_buf);	//转换成整数
							//printf("atol: %ld\n", id_src);
							if(readMsg.data.account.id == id_src)	//比较ID
							{
								memset(&writeMsg.data, 0, sizeof(writeMsg.data));
								strcpy(writeMsg.data.mtext, "ID重复，开户失败！");
								msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
								fclose(fp);
								break;
							}

							//上面生成的随机数（即卡号）与现有文件名进行比较，若重复则重新生成
							unsigned int name_num = atoi(dirp->d_name);
							if(name_num == rand_num)
							{
								rand_num = rand()%1000;	//这样重新生成后也有可能重复，这是一个小bug
							}
							fclose(fp);
						}
					}
					closedir(dir_account);	//关闭账户文件储存目录
					if(dirp != NULL)	//dirp不为空，说明有ID重复
					  break;

					//没有ID重复，则进行数据拷贝
					strncpy(writeMsg.data.account.name, readMsg.data.account.name, strlen(readMsg.data.account.name));
					strncpy(writeMsg.data.account.passwd, readMsg.data.account.passwd, 6);
					writeMsg.data.account.sex = readMsg.data.account.sex;
					writeMsg.data.account.age = readMsg.data.account.age;
					writeMsg.data.account.id = readMsg.data.account.id;
					writeMsg.data.account.money = 0;
					writeMsg.data.account.cardNum = rand_num;
					//验证卡号是否与已存在账户文件名重复，若重复则重新创建

					char account_name[10] = {0};
					sprintf(account_name, "./Account/%d", writeMsg.data.account.cardNum);//拼接文件名
					int fd_account = open(account_name, O_RDWR | O_CREAT, 0664);//创建账户文件（以随机卡号作为文件名）
					if(fd_account == -1)
					{
						perror("创建账户文件失败：");
						return -1;
					}
					//向账户文件中写入账户信息（姓名、性别、年龄、ID、卡号、密码、金额）
					dprintf(fd_account, "%s\n", writeMsg.data.account.name);//写入姓名
					dprintf(fd_account, "%c\n", writeMsg.data.account.sex);//写入性别
					dprintf(fd_account, "%d\n", writeMsg.data.account.age);//写入年龄
					dprintf(fd_account, "%ld\n", writeMsg.data.account.id);//写入ID
					dprintf(fd_account, "%d\n", writeMsg.data.account.cardNum);//写入卡号
					dprintf(fd_account, "%s\n", writeMsg.data.account.passwd);//写入密码
					dprintf(fd_account, "%d\n", writeMsg.data.account.money);//写入金额
					close(fd_account);	//关闭账户文件

					strcpy(writeMsg.data.mtext, "开户成功！");
					printf("name: %s sex: %c age: %d id: %ld card: %d passwd: %s\n", writeMsg.data.account.name, writeMsg.data.account.sex, writeMsg.data.account.age, writeMsg.data.account.id, writeMsg.data.account.cardNum, writeMsg.data.account.passwd);
					msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
					break;
				case 3:	//回应客户端的登录申请
					writeMsg.mtype = 3;	//设置发送消息类型
					if(login->flag == 1)	//如果已登录，则不允许重复登录
					{
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "当前已登录，不可重复登录！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}
					//检索目录中是否有客户端卡号命名的文件
					DIR *dir_3 = opendir("./Account");	//打开账户文件储存目录
					if(dir_3 == NULL)
					{
						perror("opendir error");
						return -1;
					}
					struct dirent *dirp_3 = NULL;
					char passwd_buf[1024] = {0};
					while((dirp_3 = readdir(dir_3)) != NULL)	//遍历账户文件目录
					{
						if(dirp_3->d_type == DT_REG)	//如果是普通文件，则读取文件的ID号
						{
							unsigned int filename_num = atoi(dirp_3->d_name);
							//printf("filename_num: %d\n", filename_num);
							//printf("cardNum: %d\n", readMsg.data.account.cardNum);
							if(filename_num == readMsg.data.account.cardNum)	//找到了账户文件
							{
								printf("find the file\n");
								char filename[256] = {0};
								sprintf(filename, "./Account/%s", dirp_3->d_name);
								FILE *fp = fopen(filename, "r");	//打开文件，验证密码是否正确
								if(fp == NULL)
								{
									perror("fopen error");
									return -1;
								}
								for(int i = 0; i < 6; ++i)	//按行读取6次，最后一次即读到passwd
								{
									memset(passwd_buf, 0, sizeof(passwd_buf));
									fgets(passwd_buf, sizeof(passwd_buf), fp);
								}

								if( strncmp(passwd_buf, readMsg.data.account.passwd, strlen(readMsg.data.account.passwd)) == 0 )//比对passwd
								{
									login->flag = 1;//passwd正确，设置登录状态	
									login->cardNum = readMsg.data.account.cardNum;	//记录卡号，即文件名
									strcpy(writeMsg.data.mtext, "登录成功！");
								}
								else
								{
									login->flag = 0;	//passwd不正确，清除登录状态
									strcpy(writeMsg.data.mtext, "登录失败，密码不正确！");
								}
								fclose(fp);

								break;	
							}
						}
					}
					if(dirp_3 == NULL)	//dirp等于NULL，说明没有账户文件
					{
						login->flag = 0;	//账户名不存在，清除登录状态
						strcpy(writeMsg.data.mtext, "登录失败，账户不存在！");
					}

					msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);	//向客户端发送信息
					break;
				case 4:	//回应客户端的存款申请
				case 5:	//回应客户端的取款申请
				case 6:	//回应客户端的查询余额申请
					if(readMsg.mtype == 4)
					{
						writeMsg.mtype = 4;	//设置发送消息类型：回应存款申请
					}
					else if(readMsg.mtype == 5)	
					{
						writeMsg.mtype = 5;	//设置发送消息类型：回应取款申请
					}
					else if(readMsg.mtype == 6)	
					{
						writeMsg.mtype = 6;	//设置发送消息类型：回应查询余额申请
					}
					if(login->flag == 0)	//如果当前未登录，则不允许存款操作
					{
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "当前未登录，不能进行存款、取款和查询操作！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}
					//已登录状态下，打开账户文件，根据客户存入/取出金额改写账户文件中的金额
					char filename_4src[256] = {0};
					char filename_4dst[256] = {0};
					char file_buf4[1024] = {0};
					int line_4 = 0;
					char money_buf4[10] = {0};
					sprintf(filename_4src, "./Account/%d", login->cardNum);
					sprintf(filename_4dst, "./Account/%d.temp", login->cardNum);
					FILE *fp_4src = fopen(filename_4src, "r");	//以只读方式打开原文件
					FILE *fp_4dst = fopen(filename_4dst, "w");	//以只写方式打开目标文件
					if((fp_4src == NULL) || (fp_4dst == NULL))
					{
						perror("fopen error");
						return -1;
					}
					while( fgets(file_buf4, sizeof(file_buf4), fp_4src) != NULL )//按行读取原文件
					{
						line_4++;
						if(line_4 == 7)	//读到金额一行
						{
							strncpy(money_buf4, file_buf4, strlen(file_buf4)+1);//保存金额（\0也一起拷贝）
							memset(file_buf4, 0, sizeof(file_buf4));
							continue;//金额这一行不写入到目标文件中
						}
						fputs(file_buf4, fp_4dst);
						memset(file_buf4, 0, sizeof(file_buf4));
					}
					int money_4src = atoi(money_buf4);
					int money_4dst = 0;
					if(writeMsg.mtype == 4)	//存款操作
					{
						money_4dst = money_4src + readMsg.data.account.money;//计算存款后的金额
						strcpy(writeMsg.data.mtext, "存款成功！");
					}
					else if(writeMsg.mtype == 5)	//取款操作
					{
						if(money_4src < readMsg.data.account.money)	//如果余额不足
						{
							money_4dst = money_4src;//余额不变
							strcpy(writeMsg.data.mtext, "余额不足，取款失败！");
						}
						else
						{
							money_4dst = money_4src - readMsg.data.account.money;//计算存款后的金额
							strcpy(writeMsg.data.mtext, "取款成功！");
						}
					}
					else if(writeMsg.mtype == 6)	//查询余额操作
					{
						money_4dst = money_4src;//余额不变
						sprintf(writeMsg.data.mtext, "查询成功！当前余额：%d", money_4src);
					}
					sprintf(file_buf4, "%d\n", money_4dst);
					fputs(file_buf4, fp_4dst);	//向新文件中写入存款后的金额
					fclose(fp_4src);	//关闭原文件
					fclose(fp_4dst);	//关闭目标文件
					remove(filename_4src);	//删除原文件
					rename(filename_4dst, filename_4src);	//将目标文件重命名为原文件名

					msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);	//向客户端发送信息
					break;
				case 7:	//回应客户端的转账申请
					writeMsg.mtype = 7;	//设置发送消息类型
					char targetfilename[256] = {0};	//用于保存目标文件名（含路径）
					if(login->flag == 0)	//如果当前未登录，则不允许转账操作
					{
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "当前未登录，不能进行转账操作！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}
					//已登录状态下
					//首先检索目录中是否有客户端卡号命名的文件
					DIR *dir_7 = opendir("./Account");	//打开账户文件储存目录
					if(dir_7 == NULL)
					{
						perror("opendir error");
						return -1;
					}
					struct dirent *dirp_7 = NULL;
					while((dirp_7 = readdir(dir_7)) != NULL)	//遍历账户文件目录
					{
						if(dirp_7->d_type == DT_REG)	//如果是普通文件，则读取文件名
						{
							unsigned int filename_num7 = atoi(dirp_7->d_name);

							if(filename_num7 == readMsg.data.account.cardNum)	//找到了目标账户文件
							{
								printf("find the targetfile\n");
								sprintf(targetfilename, "./Account/%s", dirp_7->d_name);	//拼接目标账户文件名（含路径）
								break;	
							}
						}
					}
					closedir(dir_7);	//关闭目录
					if(dirp_7 == NULL)	//等于NULL，说明没有目标账户文件
					{
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "转账失败，目标账户不存在！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}
					//找到目标文件后，对本账户进行取款操作，取款金额即转账金额（余额不足，则提示转账失败）
					char filename_7src[256] = {0};
					char filename_7dst[256] = {0};
					char file_buf7[1024] = {0};
					int line_7 = 0;
					char money_buf7[10] = {0};
					sprintf(filename_7src, "./Account/%d", login->cardNum);
					sprintf(filename_7dst, "./Account/%d.temp", login->cardNum);
					FILE *fp_7src = fopen(filename_7src, "r");	//以只读方式打开原文件
					FILE *fp_7dst = fopen(filename_7dst, "w");	//以只写方式打开目标文件
					if((fp_7src == NULL) || (fp_7dst == NULL))
					{
						perror("433 fopen error");
						return -1;
					}
					while( fgets(file_buf7, sizeof(file_buf7), fp_7src) != NULL )//按行读取原文件
					{
						line_7++;
						if(line_7 == 7)	//读到金额一行
						{
							strncpy(money_buf7, file_buf7, strlen(file_buf7)+1);//保存金额（\0也一起拷贝）
							memset(file_buf7, 0, sizeof(file_buf7));
							continue;//金额这一行不写入到目标文件中
						}
						fputs(file_buf7, fp_7dst);
						memset(file_buf7, 0, sizeof(file_buf7));
					}
					int money_7src = atoi(money_buf7);
					int money_7dst = 0;
					if(money_7src < readMsg.data.account.money)	//如果余额不足
					{
						money_7dst = money_7src;//余额不变
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "余额不足，转账失败！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}
					else
					{
						money_7dst = money_7src - readMsg.data.account.money;//计算存款后的金额
					}

					sprintf(file_buf7, "%d\n", money_7dst);
					fputs(file_buf7, fp_7dst);	//向新文件中写入存款后的金额
					fclose(fp_7src);	//关闭原文件
					fclose(fp_7dst);	//关闭目标文件
					remove(filename_7src);	//删除原文件
					rename(filename_7dst, filename_7src);	//将目标文件重命名为原文件名

					//至此，原文件中的金额改写完毕
					//下面对目标账户进行存款操作
					char filename_8dst[256] = {0};
					char file_buf8[1024] = {0};
					int line_8 = 0;
					char money_buf8[10] = {0};
					sprintf(filename_8dst, "%s.temp", targetfilename);
					FILE *fp_8src = fopen(targetfilename, "r");	//以只读方式打开原文件
					FILE *fp_8dst = fopen(filename_8dst, "w");	//以只写方式打开目标文件
					if((fp_8src == NULL) || (fp_8dst == NULL))
					{
						perror("485 fopen error");
						return -1;
					}
					while( fgets(file_buf8, sizeof(file_buf8), fp_8src) != NULL )//按行读取原文件
					{
						line_8++;
						if(line_8 == 7)	//读到金额一行
						{
							strncpy(money_buf8, file_buf8, strlen(file_buf8)+1);//保存金额（\0也一起拷贝）
							memset(file_buf8, 0, sizeof(file_buf8));
							continue;//金额这一行不写入到目标文件中
						}
						fputs(file_buf8, fp_8dst);
						memset(file_buf8, 0, sizeof(file_buf8));
					}
					int money_8src = atoi(money_buf8);
					int money_8dst = 0;	
					money_8dst = money_8src + readMsg.data.account.money;//计算存款后的金额
					sprintf(file_buf8, "%d\n", money_8dst);
					fputs(file_buf8, fp_8dst);	//向新文件中写入存款后的金额
					fclose(fp_8src);	//关闭原文件
					fclose(fp_8dst);	//关闭目标文件
					remove(targetfilename);	//删除原文件
					rename(filename_8dst, targetfilename);	//将目标文件重命名为原文件名

					memset(&writeMsg.data, 0, sizeof(writeMsg.data));
					strcpy(writeMsg.data.mtext, "转账成功！");
					msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
					break;
				case 8:	//回应客户端退出登录的申请
					writeMsg.mtype = 8;	//设置发送消息类型
					if(login->flag == 0)	//如果当前未登录，则不允许退出登录操作
					{
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "当前未登录，不能进行退出登录操作！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}
					//退出登录
					login->flag = 0;	//清空登录标志
					login->cardNum = 0;	//登录账号清零
					memset(&writeMsg.data, 0, sizeof(writeMsg.data));
					strcpy(writeMsg.data.mtext, "已退出当前登录账户！");
					msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
					break;
				case 9:	//回应客户端的销户申请
					writeMsg.mtype = 9;	//设置发送消息类型
					if(login->flag == 0)	//如果当前未登录，则不允许退出登录操作
					{
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "当前未登录，不能进行销户操作！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}
					//销户操作
					char filename_9[256] = {0};
					sprintf(filename_9, "./Account/%d", login->cardNum);
					remove(filename_9);//删除当前账户文件
					login->flag = 0;//退出当前登录
					login->cardNum = 0;
					memset(&writeMsg.data, 0, sizeof(writeMsg.data));
					strcpy(writeMsg.data.mtext, "当前账户已被销户！");
					msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
					break;
				case 10://回应客户端的退出程序申请
					writeMsg.mtype = 10;	//设置发送消息类型
					if(login->flag == 1)	//如果当前已登录，需先退出登录，再进行退出程序操作
					{
						memset(&writeMsg.data, 0, sizeof(writeMsg.data));
						strcpy(writeMsg.data.mtext, "当前已登录，不能进行退出程序操作！");
						msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
						break;
					}
					//向父进程发送信号
					int ret_kill = kill(0, SIGUSR1);
					if(ret_kill == -1)
					{
						perror("kill error");
						exit(-1);
					}
					memset(&writeMsg.data, 0, sizeof(writeMsg.data));
					strcpy(writeMsg.data.mtext, "已退出程序！");
					msgsnd(msgid_server, &writeMsg, sizeof(writeMsg.data), 0);
					break;

				default:
					printf("非有效输入！\n");
			}

			exit(0);
		}
		else if(pid > 0)	//父进程继续阻塞等待读取消息队列
		{
			//回收已经退出的子进程
			while( waitpid(-1, NULL, WNOHANG) > 0 );
		}
	}

	while( waitpid(-1, NULL, WNOHANG) != -1 );	

	int ret = shmdt(login);
	if(ret == -1)
	{
		perror("shmdt error");
		return -1;
	}
	
	ret = shmctl(shmid, IPC_RMID, NULL);
	if(ret == -1)
	{
		perror("shmctl error");
		return -1;
	}

process_exit:

	ret = msgctl(msgid_server, IPC_RMID, NULL);
	if(ret == -1)
	{
		perror("msgctl server error");
		return -1;
	}
	ret = msgctl(msgid_client, IPC_RMID, NULL);
	if(ret == -1)
	{
		perror("msgctl client error");
		return -1;
	}

	unlink(MSG_SERVER_FILE);
	unlink(MSG_CLIENT_FILE);

	return 0;
}
