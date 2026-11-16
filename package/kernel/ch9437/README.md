# CH9437F 八串口扩展驱动

MT7628NN 平台 CH9437F 串口扩展芯片 Linux 内核驱动，提供 7 路扩展串口，每路以独立设备节点暴露给用户态，支持 RS485 收发方向自动切换。

---

## 一、系统架构

```
L4 应用层      read/write/ioctl(/dev/gps, /dev/ws63, /dev/rs485 ...)
   │            获取数据 / 配置设备 / 查看状态（协议解析在用户态）
   ▼
L3 设备驱动层  ch9437-dev.ko: serdev 传输型驱动
   │            每个 /dev/<节点> 对应一个 CH9437 子串口
   ▼
L2 串口提供层  ch9437.ko: 全轮询命令引擎 + serdev 控制器
   │            500Kbps 主机链路 / 10ms 轮询 worker / 485 方向控制
   ▼
L1 硬件        MT7628 UART1 ←500Kbps→ CH9437 ← 各子串口 → 外设
```

### 通道分配

| 设备节点 | 子串口 | 对端外设 | RS485 方向控制 | 默认波特率 |
|---------|--------|----------|---------------|-----------|
| /dev/ws63 | 串口1 | M528H-WS63 星闪模组 | — | 9600 |
| /dev/rs485 | 串口2 | RS485 收发器 #1 (SP3485) | TNOW2 (PIN1, 硬件自动) | 9600 |
| /dev/rv1106 | 串口3 | RV1106 | — | 9600 |
| /dev/mcu3 | 串口4 | N32S032JQ1 MCU·UART3 | — | 9600 |
| /dev/mcu0 | 串口5 | N32S032JQ1 MCU·UART0 | — | 9600 |
| /dev/rs485_6 | 串口6 | RS485 收发器 #2 (SP3485) | GPIO5 (PIN3, 软件手动) | 9600 |
| /dev/gps | 串口7 | ATGM336H-5N31 GPS | — | 9600 |

### 关键硬件参数

| 参数 | 值 | 说明 |
|------|---|------|
| 主机链路波特率 | 500,000 bps | MT7628 分频 N=5 / CH9437 D=384，双方整数精确零误差 |
| CH9437 时钟 | 外部 16MHz 晶振 | 初始化时切换（需先使能 HSE 引脚） |
| CH9437 基准时钟 | 192 MHz | 16MHz × 12 倍频 |
| 主机链路 | UART1 @ 0x10000D00 | 16550 兼容，IRQ 21（未使用，见问题 7） |
| CH9437 中断 INT# | 接 MT7628 GPIO0 (PIN30) | 预留（当前轮询架构未使用） |

---

## 二、解决的问题

### 问题 1：内核配置缺少 serdev 总线

**现象**：模块编译时报 `serdev_device_open` 等符号未定义。

**原因**：`CONFIG_SERIAL_DEV_BUS` 未在内核 `.config` 中启用。

**修复**：在 `target/linux/ramips/config-4.14` 中添加：
```
CONFIG_SERIAL_DEV_BUS=y
# CONFIG_SERIAL_DEV_CTRL_TTYPORT is not set
```

注意：`SERIAL_DEV_CTRL_TTYPORT` 是新出现的 Kconfig 选项，无默认值会导致 `silentoldconfig` 交互中止，必须显式关闭。

---

### 问题 2：头文件缺少 DLL/DLM 宏定义

**现象**：编译报 `'CH9437_UDLL' undeclared`。

**原因**：`ch9437.h` 中遗漏了子串口波特率除数锁存器宏。

**修复**：在头文件中补充：
```c
#define CH9437_UDLL    0x00    /* DLAB=1 */
#define CH9437_UDLM    0x01    /* DLAB=1 */
```

---

### 问题 3：包含路径错误

**现象**：编译报 `linux/ioctls.h: No such file or directory`。

**原因**：内核 4.14 中不存在 `include/linux/ioctls.h`，TIOCSRS485 等定义在 `asm/ioctls.h` 中。

**修复**：将 `#include <linux/ioctls.h>` 改为 `#include <asm/ioctls.h>`。

---

### 问题 4：内核模块 Makefile 目标名与源文件名不匹配

**现象**：编译报 `No rule to make target 'ch9437-dev.c', needed by 'ch9437-dev.o'`。

**原因**：`obj-m += ch9437-dev.o` 使用连字符，而源文件名为 `ch9437_dev.c`（下划线）。

**修复**：使用复合模块规则：
```makefile
obj-m += ch9437-dev.o
ch9437-dev-y := ch9437_dev.o
```

---

### 问题 5：io_sel 引脚使能应答字节位置歧义

**现象**：驱动 probe 时报 `io_sel cmd=03 addr=09: no 0x5A ack`，CH9437 引脚使能失败。

**原因**：手册描述"数据内容第三字节为状态标志"，4 字节寄存器布局为 `[命令码][数据0][数据1][数据2]`，"第三字节"指数据部分的第三字节（即整体第四字节 rd[3]），初始代码检查的是 rd[2]。

**修复**：宽容检查，接受 rd[2] 或 rd[3] 任一为 0x5A：
```c
if (got >= 3 && (rd[2] == 0x5A || rd[3] == 0x5A))
    return 0;
```

实测响应格式确认为 `[cmd][addr][enable][0x5A]`，状态字节在 rd[3]。

---

### 问题 6：外部晶振引脚未使能导致时钟切换后链路丢失

**现象**：切换外部 16MHz 时钟后版本寄存器读取超时。

**原因**：CH9437 上电后 XI/XO 引脚默认浮空，必须先通过 IO_SEL（默认引脚地址 9 = HSE_XI_XO）使能晶振引脚，晶振起振后才能切换时钟源。否则芯片失去时钟，整个链路瘫痪。

**修复**：在时钟切换前增加 HSE 引脚使能 + 起振等待：
```c
/* 使能 XI/XO 引脚 (手册 8.10, 默认引脚地址 9) */
ret = ch9437_io_sel_raw(chip, CH9437_IOSEL_SET_DEF,
                        CH9437_DEF_PIN_HSE, 1);
msleep(10);  /* 等待晶振起振 */

/* 然后才能切换时钟源 */
v = 0x01;
ch9437_raw_reg_write(chip, CH9437_R_CLK_CTRL, &v, 1);
```

---

### 问题 7：MT7628 UART-Lite2 中断线路硬件缺陷（最核心问题）

**现象**：`request_irq` 后系统立即硬死锁，看门狗复位重启。ISR 以 `iir=0xC4 lsr=60 ier=01` 的状态无限风暴（IIR 报 THRE 挂起，IER 仅为 RDI=0x01 未使能 THRI，LSR 无接收数据）。

**排查过程**：

| 实验 | 结果 | 结论 |
|------|------|------|
| IER 回读 | `ier=01`（THRI=0） | IER 写入正确，非软件误使能 |
| MCR OUT2 置位 | 无效 | 非中断输出门控问题 |
| IIR/MSR 读清 + 8250 式四连读 | 无效 | 非常规锁存问题 |
| INTC RAW_STATUS 差分实验 | `base=00000000, pend=00200000, after=00000000` | hwirq 21 确实是该 UART 的线；无其他干扰源；RX 中断行为正常 |
| 空闲 IIR 读取 | `uart_iir_idle=c1`（干净） | 非空闲假象，THRE 挂起是真实的 |
| INTC STATUS 读取 | `st0=00000000`（INTC 层面无挂起） | 矛盾：UART 内部报挂起但 INTC 层不显示 |

**根因**：MT7628 的 UART-Lite2（0xD00）IP 实例存在硬件缺陷——**只要 IER≠0 且发送保持寄存器（THR）为空，中断输出线就持续有效，不受 THRI 使能位门控**。这是该 IP 变体特有的行为（同 SoC 上 UART-Lite @0xC00 做控制台时无此问题）。

上游 dtsi 中启用 uart1 的其他板子未暴露此问题，因为 8250 驱动只在 tty 被 open 时才 `request_irq`，那些板子从未打开过 ttyS1。

**解决**：**放弃使用 UART 中断，改为全轮询命令引擎架构**。

由于 CH9437 主机链路是严格的命令-响应协议（芯片从不主动发送），轮询与中断的 CPU 成本完全相同：

```c
/* 全轮询命令引擎核心 */
static int ch9437_cmd_xmit(...)   /* 轮询 THRE，每次填 16 字节到 THR */
static int ch9437_cmd_recv(...)   /* 轮询 DR，逐字节收响应 */

int ch9437_cmd_exec(...)
{
    /* 200ms 超时 × 3 次重试，命令前清除残留 RX */
    /* 发送帧 → 同步等待响应 → 返回 */
}
```

空闲时 IER 恒为 0 → 中断线恒低电平 → **风暴结构性不可能发生**。

---

### 问题 8：INTC 诊断 ioremap 物理地址与虚拟地址混淆

**现象**：INTC 寄存器读取全部返回 `0xBADDBADD`（MIPS 总线对未映射地址的魔法值）。

**原因**：将 KSEG1 虚拟地址 `0xB0000200` 当作物理地址传给 `ioremap_nocache()`，而 INTC 的实际物理地址是 `0x10000200`（palmbus 0x10000000 + 偏移 0x200）。

**修复**：
```c
/* 错误: ioremap_nocache(0xb0000200, 0x100)  ← KSEG1 虚拟地址 */
/* 正确: */
chip->intc_diag = ioremap_nocache(0x10000200, 0x100);  /* 物理地址 */
```

**经验**：MIPS 架构中 `ioremap()` 接受物理地址，kseg0/kseg1 虚拟地址（0x80000000/0xA00000000 段）是 MMU 直映射，不能作为 ioremap 参数。

---

### 问题 9：ch9437_reg_read 返回值约定不统一（GPS 数据流不出的直接原因）

**现象**：驱动 probe 成功、7 个设备节点全部创建、set_baud 正常，但 `head -c 500 /dev/gps` 无任何数据输出。

**原因**：`ch9437_cmd_exec` 对读命令成功时返回接收字节数（如 1），而 worker 中所有检查都按"0=成功"的约定编写：

```c
/* worker 中的检查 */
if (ch9437_reg_read(chip, CH9437_R_FIFO_CNT_L, &lo, 1))
    continue;    /* 成功返回 1 → 条件为真 → 跳过该通道！ */
```

**每一轮 worker 都在"成功读取 FIFO 计数"后将该通道跳过**——RX 永远不读、TX 永远不推。而 `reg_write` 成功恰好返回 0（rlen=0），所以 probe 阶段的 set_baud 等写命令全部正常，掩盖了这个 bug。

**修复**：统一 `ch9437_reg_read` 返回 0 表示成功：
```c
int ch9437_reg_read(struct ch9437_chip *chip, u8 reg, u8 *buf, int len)
{
    u8 frame[3];
    int ret;

    ch9437_mk_hdr(frame, reg);
    ret = ch9437_cmd_exec(chip, frame, 3, buf, len);
    return (ret == len) ? 0 : ret;    /* 0=成功, 负数=失败 */
}
```

此修复同时激活了 RBR 批量读、THR 批量写、RS485 GPIO 方向释放、`port_drained` 等所有依赖 `reg_read` 返回值判断的功能。

---

### 问题 10：RS485 方向控制引脚与子串口不匹配（硬件设计问题）

**现象**：串口6 的 RS485 方向控制引脚 TNOW4（PIN3）按手册属于 UART4，不跟随串口6 的发送。

**原因**：CH9437 的 TNOW 引脚与 UART 的对应关系是固定的（TNOW_n 属于 UART_n），但硬件设计时将串口6 的 485 方向控制接到了 PIN3（TNOW4），而 TNOW4 只跟随 UART4 的发送。串口2 的方向控制接在 PIN1（TNOW2）恰好正确。

**修复**：软件方案——将 PIN3 配置为 GPIO5（而非 TNOW4 功能），驱动在串口6 发送前后手动控制方向：

```
发送前: GPIO5 拉高 (DE 使能)
发送数据 → 等 FIFO 排空 + LSR.TEMT=1 (移位寄存器也发完)
发送后: GPIO5 拉低 (回到接收态)
```

同时确保不使能 TNOW4 功能（IO_SEL 复用地址 6），防止串口4（MCU UART3）的数据流误触发 485 #2 驱动器。

---

## 三、使用指南

### 用户态 API

```c
int fd = open("/dev/gps", O_RDWR);

/* 读数据（阻塞，支持 poll/select） */
read(fd, buf, len);

/* 写数据（透传到对端设备） */
write(fd, "$PCAS03,1*1F\r\n", 14);  /* 例: GPS 配置命令 */

/* ioctl */
ioctl(fd, CHDEV_IOC_GET_STATS, &stats);   /* 收发统计 */
ioctl(fd, CHDEV_IOC_GET_STATUS, &status); /* 通道状态 */
ioctl(fd, CHDEV_IOC_SET_BAUD, &baud);     /* 动态改波特率 */
ioctl(fd, CHDEV_IOC_FLUSH);               /* 清接收缓冲 */

close(fd);
```

### ioctl 定义（ch9437_dev.h）

```c
#define CHDEV_IOC_MAGIC 'C'

struct chdev_stats {
    __u64 rx_bytes;
    __u64 tx_bytes;
    __u32 overruns;
    __u32 ch;
    __u32 baud;
};

struct chdev_status {
    __u32 ch;
    __u32 baud;
    __u32 opened;
    __u32 reserved;
};

#define CHDEV_IOC_GET_STATS   _IOR(CHDEV_IOC_MAGIC, 1, struct chdev_stats)
#define CHDEV_IOC_GET_STATUS  _IOR(CHDEV_IOC_MAGIC, 2, struct chdev_status)
#define CHDEV_IOC_SET_BAUD    _IOW(CHDEV_IOC_MAGIC, 3, __u32)
#define CHDEV_IOC_FLUSH       _IO(CHDEV_IOC_MAGIC, 4)
```

### 快速验证

```bash
head -c 200 /dev/gps          # GPS NMEA 语句流
cat /dev/rs485                # 485 数据（需接对端设备）
echo "AT" > /dev/ws63         # 向星闪模组发 AT 命令
```

### 调试口（可选）

无设备子节点的通道自动回落为标准 tty 调试口（/dev/ttyCH\<n\>），支持 `stty` 设置波特率：

```bash
stty -F /dev/ttyCH3 115200    # 设置串口3 波特率
cat /dev/ttyCH3               # 读取数据
```

---

## 四、设备树配置

```dts
&uart1 {
    compatible = "wch,ch9437f";
    status = "okay";
    current-speed = <500000>;        /* 主机链路波特率 */
    wch,external-clock;              /* 使用外部 16MHz 晶振 */
    #address-cells = <1>;
    #size-cells = <0>;

    serial@1 {
        reg = <1>;                    /* 子串口号 1-7 */
        ws63 { compatible = "custom,m528h-ws63"; };
    };

    serial@2 {
        reg = <2>;
        wch,rs485-auto;              /* TNOW2 硬件自动方向 */
        rs485 { compatible = "custom,rs485"; };
    };

    serial@6 {
        reg = <6>;
        wch,rs485-gpio = <5>;        /* GPIO5 软件方向控制 */
        rs485 { compatible = "custom,rs485"; };
    };

    serial@7 {
        reg = <7>;
        current-speed = <9600>;      /* 子串口默认波特率 */
        gps { compatible = "custom,atgm336h"; };
    };
};
```

### 新增外设

在对应 `serial@n` 节点下添加子节点即可，`ch9437-dev` 驱动自动匹配并创建设备节点：

```dts
serial@3 {
    reg = <3>;
    my_device { compatible = "custom,my-device"; };
};
```

设备节点名为 DT 子节点名（重复名称自动加 `_通道号` 后缀）。

---

## 五、构建

```bash
# 完整固件
make -j$(nproc)

# 仅编译驱动模块（迭代调试用）
make package/kernel/ch9437/compile V=s
make package/kernel/ch9437-dev/compile V=s

# 产物
# bin/targets/ramips/mt76x8/openwrt-ramips-mt76x8-mt7628dan_dev-squashfs-sysupgrade.bin
```

### 模块热替换（开发迭代）

```bash
scp build_dir/.../ch9437-1.0/ch9437.ko root@192.168.50.1:/tmp/
ssh root@192.168.50.1 "rmmod ch9437; insmod /tmp/ch9437.ko"
```

重新探测安全：芯片保持 500K 锁定，0x55 自动波特率重发被忽略，HSE/时钟切换等操作幂等。

---

## 六、包结构

```
package/kernel/ch9437/               kmod-ch9437 (L2 提供者)
├── Makefile                         OpenWrt 内核包定义
└── src/
    ├── ch9437.h                     寄存器/帧格式/数据结构
    ├── ch9437_core.c                轮询命令引擎 + 初始化 + worker
    ├── ch9437_tty.c                 tty 调试回落口 + termios + ioctl
    └── ch9437_serdev.c              serdev 控制器

package/kernel/ch9437-dev/           kmod-ch9437-dev (L3 传输)
├── Makefile
└── src/
    ├── ch9437_dev.h                 ioctl 定义（用户态共享）
    └── ch9437_dev.c                 serdev 消费者 + 字符设备
```

---

## 七、性能参数

| 指标 | 值 |
|------|---|
| 主机链路波特率 | 500 Kbps（可配置，最大 2.5M） |
| 子串口波特率范围 | 3600 ~ 12 Mbps（4 位小数分频） |
| 每通道 RX FIFO | 2048 字节 |
| 每通道 TX FIFO | 1536 字节 |
| 轮询周期 | 10ms（模块参数 poll_ms 可调） |
| 单次 RBR 批量读 | 最大 2048 字节 |
| 单次 THR 批量写 | 最大 1536 字节 |
| 命令超时 | 200ms × 3 次重试 |
| CPU 占用（7 通道全开空闲） | < 5% |

---

## 八、已知限制与后续优化

| 项目 | 说明 |
|------|------|
| 轮询延迟 | 最高 10ms（poll_ms 可调至 1ms，代价是 CPU 占用上升） |
| CH9437 INT# 未使用 | 板上已接 GPIO0 (PIN30)，可作为轮询唤醒触发（降低延迟和 CPU 占用） |
| AB 双槽固件 | 需用 `mt7628dan_config/ab.diffconfig` 单独构建 |
| 500K 带宽 | 7 通道共享约 400K 有效吞吐；若 WS63 需持续 460800 可改 DT current-speed 为 2500000 |
