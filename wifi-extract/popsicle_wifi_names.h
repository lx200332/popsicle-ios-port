/* 自动生成：来源见 wifi-extract/ 下的真机提取物（gen_header.py 重跑即可），勿手改 */
#ifndef POPSICLE_WIFI_NAMES_H
#define POPSICLE_WIFI_NAMES_H

/* ---- PCI 身份 ---- */
#define PCI_VENDOR_QCOM          0x17cb
#define PCI_DEV_WCN7850_PEACH    0x110e
#define PCI_CLASS_NETWORK_OTHER  0x028000
#define PCI_EP_BUSDEVFN           0x000800   /* 01:00.0 */
#define PCI_RP_BUSDEVFN           0x000000   /* 00:00.0 */

/* ---- BAR（实测）---- */
#define BAR0_PHYS   0x40400000ULL   /* 2 MiB，MHI doorbell + BHI 寄存器 */
#define BAR0_SIZE   0x00200000ULL
#define BAR2_PHYS   0x40300000ULL   /* 32 KiB，MSI-X 表 */
#define BAR2_SIZE   0x00008000ULL
#define MSIX_TABLE_OFF  0x0000      /* MSI-X table = BAR2 + 0 */
#define MSIX_PBA_OFF    0x1000      /* PBA = BAR2 + 0x1000 */
#define MSIX_VECTORS    32

/* ---- PCIe 寄存器窗（DT reg，六段）---- */
#define PCIE_PARF_PHYS   0x01c00000ULL  /* size 0x3000 */
#define PCIE_PHY_PHYS    0x01c06000ULL  /* size 0x2000 */
#define PCIE_DM_PHYS     0x400000000ULL /* size 0x10000000 (DBI 前 4K) */
#define PCIE_ELBI_PHYS   0x40f200000ULL /* size 0xa8 —— 单独访问会死机 */
#define PCIE_IATU_PHYS   0x410000000ULL /* size 0x1000 —— 出站映射 */
#define PCIE_SM_PHYS     0x01d07000ULL  /* size 0x7000 */
#define PCIE_PHY_STATUS_OFF   0x414
#define PCIE_PHY_STATUS_BIT   7
#define PCIE_SM_START_OFF     0x1090
#define PCIE_SM_BRANCH_OFF    0x1000
#define PCIE_PHY_SEQ_TUPLES   199   /* qcom,phy-sequence 2388 B / 12 */

/* ---- GDSC ---- */
#define GCC_PHYS              0x00100000ULL
#define GCC_SIZE              0x00080000ULL
#define GCC_PCIE_GDSC_OFF     0x6b004
#define GCC_PCIE_PHY_GDSC_OFF 0x6c000
#define GDSC_SW_COLLAPSE      1
#define GDSC_CFG_OFF          4
#define GDSC_POWER_UP_COMPLETE   0x10000
#define GDSC_POWER_DOWN_COMPLETE 0x8000

/* ---- TLMM（每 pin 一个 4 KiB 窗口，不是 0x10 步长）---- */
#define TLMM_PHYS     0x0f100000ULL
#define TLMM_STRIDE   0x1000
#define TLMM_SIZE     0x000ce000ULL
#define GPIO_WLAN_EN    16
#define GPIO_SW_CTRL    18   /* WiFi/BT 共用 */
#define GPIO_WLAN_SW    19
#define GPIO_PERST      102
#define GPIO_CLKREQ     103  /* 专用功能 pcie0_clk_req_n，另见 offset 0x2c48 */
#define GPIO_WAKE       104
#define GPIO_HOST_SOL   204
#define GPIO_DEV_SOL    205

/* ---- MSI-X 向量分配（实测，固定）---- */
#define MSIX_VEC_BHI        0
#define MSIX_VEC_MHI_A      1
#define MSIX_VEC_MHI_B      2
#define MSIX_VEC_CE_BASE    4    /* ce_1..ce_8 -> vec 4..11，vec 8 未绑 */
#define MSIX_VEC_CE_MISSING 8
#define MSIX_VEC_WAKE       13
#define MSIX_VEC_DP_BASE    14   /* dp_0..dp_17 -> vec 14..31，vec 18/22 未绑 */

/* ---- 时钟（实测）---- */
#define RF_CLK_HZ        76800000
#define PCIE_PIPE_CLK_HZ 19200000

/* ---- PCIe 电源/链路 ---- */
#define PCIE_TARGET_LINK_SPEED 3   /* Gen3 = 8 GT/s，实测只训到 2.5~5 GT/s x1 */
#define PCIE_AUX_CLK_MHZ       20
#define PCIE_EP_LATENCY        10
#define PCIE_L1SS_TIMEOUT_US   5000

/* ---- 固件 ---- */
#define FW_AMSS_PATH   "peach/amss20.bin"
#define FW_AMSS_SIZE   7319616
#define FW_AMSS_ENTRY  0x0180a800   /* ELF32 EM_ARM，7 段 */
#define FW_AMSS_SEGMENTS 7
#define FW_REGDB_PATH  "peach/regdb_xiaomi.bin"
#define FW_REGDB_SIZE  24278
#define FW_BDF_PATH    "peach/bd_p2.elf"
#define FW_BDF_SIZE    125288
#define FW_QDSS_PATH   "peach/qdss_trace_config_v2.cfg"
#define FW_QDSS_SIZE   6245
#define FW_CFG_INI     "wlan/qca_cld/peach_v2/WCNSS_qcom_cfg.ini"
#define FW_RAMDUMP_SIZE 0x600000    /* 6 MiB，从 CMA 里切 */

/* ---- CMA / IOMMU ---- */
#define CNSS_CMA_SIZE     0x02000000ULL  /* 32 MiB，必须连续 */
#define CNSS_IOVA_BASE0   0x00000000ULL
#define CNSS_IOVA_SIZE0   0x18000000ULL
#define CNSS_IOVA_BASE1   0xb0000000ULL
#define CNSS_IOVA_SIZE1   0x50000000ULL
#define CNSS_SMMU_SID_BASE 0x1400
#define CNSS_IPA_PHYS     0xb0000000ULL  /* smmu_iova_ipa，size 0x10000 */

/* ---- QRTR / QMI ---- */
#define QRTR_MHI_DEV_ID  0x1111
#define QRTR_MHI_NET_ID  1

/* ---- 实机固件自报的版本（用于校验你抓到的是同一份）---- */
#define FW_VERSION_BUILD  "2003ffff"
#define FW_BOARD_VERSION  8, 0, 0, 16, 1
#define FW_IMAGE_VARIANT  "7880.wlanfw.eval_v2_TOQ"
#define MHI_SERIAL_NUMBER 3216895231

/* ---- BHI 块（相对 bhi_off；bhi_off = read32(BAR0 + 0x28)）---- */
#define MHI_REG_BHIOFF            0x28
#define MHI_REG_BHIEOFF           0x2c
#define BHI_BHIVERSION_MINOR      0x00
#define BHI_BHIVERSION_MAJOR      0x04
#define BHI_IMGADDR_LOW           0x08
#define BHI_IMGADDR_HIGH          0x0c
#define BHI_IMGSIZE               0x10
#define BHI_IMGTXDB               0x18   /* write session_id to kick */
#define BHI_INTVEC                0x20
#define BHI_EXECENV               0x28
#define BHI_STATUS                0x2c   /* bits 31:30 = status */
#define BHI_ERRCODE               0x30
#define BHI_ERRDBG1               0x34
#define BHI_ERRDBG2               0x38
#define BHI_ERRDBG3               0x3c
#define BHI_SERIALNU              0x40
#define BHI_SBLANTIROLLVER        0x44
#define BHI_NUMSEG                0x48
#define BHI_MSMHWID(n)            (0x4c + 4 * (n))
#define BHI_OEMPKHASH(n)          (0x64 + 4 * (n))

#define BHI_STATUS_MASK           0xc0000000u   /* bits 31:30 */
#define BHI_STATUS_RESET          0x00
#define BHI_STATUS_SUCCESS        0x02
#define BHI_STATUS_ERROR          0x03
#define BHI_TXDB_SEQNUM_MASK      0x3fffffffu

/* ---- BHIE 块（相对 bhie_off；bhie_off = read32(BAR0 + 0x2c)）---- */
#define BHIE_MSMSOCID_OFFS        0x00
#define BHIE_TXVECADDR_LOW_OFFS   0x2c
#define BHIE_TXVECADDR_HIGH_OFFS  0x30
#define BHIE_TXVECSIZE_OFFS       0x34
#define BHIE_TXVECDB_OFFS         0x3c
#define BHIE_TXVECSTATUS_OFFS     0x44
#define BHIE_RXVECADDR_LOW_OFFS   0x60
#define BHIE_RXVECADDR_HIGH_OFFS  0x64
#define BHIE_RXVECSIZE_OFFS       0x68
#define BHIE_RXVECDB_OFFS         0x70
#define BHIE_RXVECSTATUS_OFFS     0x78

#define BHIE_STATUS_MASK          0xc0000000u
#define BHIE_STATUS_RESET         0x00
#define BHIE_STATUS_XFER_COMPL    0x02
#define BHIE_STATUS_ERROR         0x03

/* struct bhi_vec_entry —— 16 字节，不是 8 */
#define BHI_VEC_ENTRY_SIZE        16

/* ---- MHI 主块（相对 BAR0 基址）---- */
#define MHIREGLEN                 0x00
#define MHIVER                    0x08
#define MHICFG                    0x10
#define CHDBOFF                   0x18
#define ERDBOFF                   0x20
#define MHICTRL                   0x38
#define MHISTATUS                 0x48
#define CCABAP_LOWER              0x58
#define CCABAP_HIGHER             0x5c
#define ECABAP_LOWER              0x60
#define ECABAP_HIGHER             0x64
#define CRCBAP_LOWER              0x68
#define CRCBAP_HIGHER             0x6c
#define CRDB_LOWER                0x70
#define CRDB_HIGHER               0x74
#define MHICTRLBASE_LOWER         0x80
#define MHICTRLLIMIT_LOWER        0x88
#define MHIDATABASE_LOWER         0x98
#define MHIDATALIMIT_LOWER        0xa0

#define MHICFG_NHWER_MASK         0xff000000u
#define MHICFG_NER_MASK           0x00ff0000u
#define MHICFG_NHWCH_MASK         0x0000ff00u
#define MHICFG_NCH_MASK           0x000000ffu
#define MHICTRL_RESET_MASK        0x00000002u
#define MHICTRL_MHISTATE_MASK     0x0000ff00u
#define MHISTATUS_READY_MASK      0x00000001u
#define MHISTATUS_SYSERR_MASK     0x00000004u
#define MHISTATUS_MHISTATE_MASK   0x0000ff00u

/* ---- 固件投递参数（来自 ath12k-mhi.c L256-258）---- */
#define FW_IMAGE_SIZE         7319616u   /* peach/amss20.bin 实测 */
#define FW_SBL_SIZE           0x00080000u /* 524288 == 内层 Hexagon ELF 头偏移 */
#define FW_SEG_LEN            0x00080000u
#define FW_FBC_DOWNLOAD       1
#define FW_SEG_COUNT          13          /* ceil((7319616-524288)/524288) */
#define FW_SEG_LAST_SIZE      503872u
#define FW_VEC_TABLE_SIZE     (FW_SEG_COUNT * BHI_VEC_ENTRY_SIZE)   /* 208 B */

/* EE 状态（判固件活了看是否 == MHI_EE_WFW）*/
#define MHI_EE_PBL            0
#define MHI_EE_SBL            1
#define MHI_EE_AMSS           2   /* "MISSION MODE" */
#define MHI_EE_RDDM           3
#define MHI_EE_WFW            4   /* "WLAN FIRMWARE" —— 目标状态 */
#define MHI_EE_PTHRU          5
#define MHI_EE_EDL            6
#define MHI_EE_FP             7
#define MHI_EE_DISABLE_TRANSITION 8

#endif /* POPSICLE_WIFI_NAMES_H */

