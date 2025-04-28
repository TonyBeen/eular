# 时间不同步的情况下计算的RTT不准确
在不使用NTP同步时间的情况下只能计算相对时间. (参考TCP)
1、A记录发送的时间t0
2、B收到探测包后记录到达时间t1, 并回复响应, 携带着到达时间t1及响应时间t2
3、A收到响应包记录收到包的时间为t3, RTT = t3 - t0 - (t2 -t1)

### NOTE 重传包不参与RTT计算, 由于无法确定重传包的ACK是哪次传输的, 故计算不准确

然而, 在存在丢包或乱序的情况下, 则可能引入较大的误差. 需要引入加权移动平均法(TCP采用的算法)更新RTT估算值
[指数加权移动平均详解](https://blog.csdn.net/qq_42363032/article/details/117352591)
[[通俗易懂]深入理解TCP协议（下）：RTT、滑动窗口、拥塞处理](https://www.imooc.com/article/29368)

linux/net/ipv4/tcp_input.c:tcp_rtt_estimator L820