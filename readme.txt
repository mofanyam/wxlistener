第三方依赖：
libev

使用:
./wxlistener ip port path2worker [workercount [shmsize]]


向worker传递的环境变量：
LISTEN_FD=xxx
SHM_ID=xxx
SHM_SIZE=xxx
WKR_ID=xxx
WKR_COUNT=xxx


平滑关闭: kill -QUIT {wxlistener-pid}
平滑重启: kill -HUP {wxlistener-pid}


如果不是接收到来自wxlistener的-QUIT信号，在worker进程退出时都将会有新的worker进程被启动以补充
如果worker重启的时间距离上一次小于2秒，则放弃重启
