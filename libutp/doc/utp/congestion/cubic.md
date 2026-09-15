# Cubic 拥塞控制算法

## 1. 概述
Cubic 是 Linux 内核默认使用的拥塞控制算法。它通过一个三次函数（Cubic Function）来调整拥塞窗口（CWND）。与传统 TCP Reno 的线性增长不同，Cubic 的窗口增长速度取决于距离上一次丢包事件的时间，这使得它在长距离高带宽网络（LFN, Long Fat Networks）中具有更好的扩展性。

## 2. 核心数学模型
Cubic 的窗口增长函数为：
$$W(t) = C(t - K)^3 + W_{max}$$

其中：
*   **W(t)**：当前的拥塞窗口。
*   **C**：比例常数（Cubic Parameter），`libutp` 默认为 0.4。
*   **t**：距离上一次窗口减小经过的时间。
*   **W_max**：发生拥塞（丢包）时的窗口大小。
*   **K**：函数到达 $W_{max}$ 所需的时间，计算公式为 $K = \sqrt[3]{W_{max} \cdot (1 - \beta) / C}$。
*   **$\beta$**：乘性减小因子，`libutp` 默认为 0.7。

## 3. 运行流程

### 3.1 慢启动阶段 (Slow Start)
*   **行为**：指数增长。每收到一个 ACK，CWND 增加一个 MSS。
*   **退出条件**：当 CWND 达到慢启动阈值 (`ssthresh`) 时，进入拥塞避免阶段。

### 3.2 拥塞避免阶段 (Congestion Avoidance)
在这个阶段，Cubic 根据三次曲线调整窗口，主要分为三个区域：

1.  **Reno 友好区 (TCP-Friendly Region)**：
    *   如果 Cubic 计算出的窗口增长速度慢于标准 TCP，则强制切换到线性增长，确保在低带宽下不被传统 TCP 欺负。
2.  **凹面增长区 (Concave Region)**：
    *   当窗口刚从丢包中恢复，距离 $W_{max}$ 较远时，窗口快速增长；随着接近 $W_{max}$，增长速度放缓。
3.  **凸面增长区 (Convex Region)**：
    *   一旦窗口超过了上一次的 $W_{max}$ 且没有发生丢包，说明网络带宽可能增加了。此时窗口会由慢到快地加速增长，以探测新的瓶颈带宽。

### 3.3 丢包处理 (Multiplicative Decrease)
*   当检测到丢包时：
    1.  更新 $W_{max}$ 为当前的 CWND 值。
    2.  执行乘性减小：`CWND = CWND * beta` (通常为 0.7)。
    3.  重置纪元时间（Epoch Start），重新开始三次曲线计算。

## 4. libutp 实现特点
*   **RTT 独立性**：Cubic 的窗口增长主要取决于时间 $t$，而不是 RTT 的次数。这意味着在不同 RTT 的流之间，Cubic 具有更好的公平性。
*   **混合慢启动 (Hybrid Slow Start)**：虽然代码库中以标准三次函数为主，但其实现保证了在 CWND 较大时的平滑过渡。
*   **Pacing 支持**：虽然 Cubic 通常不需要像 BBR 那样强制 Pacer，但 `libutp` 的实现中 `getPacingRate()` 依然会返回一个基于 `CWND / RTT` 的估算速率，以辅助 Pacer 平滑发包。
