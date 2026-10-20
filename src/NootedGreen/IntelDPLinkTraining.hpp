// SPDX-License-Identifier: MIT
//
// IntelDPLinkTraining.hpp — Tiger Lake combo PHY DP voltage swing & pre-emphasis
//
// Canonical register names from linux_mmio_mapper pipeline (approved_source_confirmed).
// Linux reference: drivers/gpu/drm/i915/display/intel_ddi.c
//                  icl_ddi_combo_vswing_program() / icl_combo_phy_set_signal_levels()
//
// PHY assignments (TGL combo PHY):
//   PHY_A → ports 0 (eDP / internal)  base 0x162000
//   PHY_B → port  1 (DP/HDMI-B)       base 0x06C000
//
// Only combo PHY (A/B) is handled here.
// TC/MG PHY (C-F) use tgl_dkl_phy_set_signal_levels — not implemented yet.

#pragma once
#include <stdint.h>

// ─── Combo PHY base addresses ────────────────────────────────────────────────
// ICL / TGL / ADL-P share the same DW-register layout at these bases.
// ADL-P ports A-B = combo PHY; ports C-F = DKL PHY (separate impl, not here).
#define _ICL_COMBOPHY_A             0x162000u   // PHY 0 — port A (eDP / internal)
#define _ICL_COMBOPHY_B             0x06C000u   // PHY 1 — port B (DP/HDMI-B)
#define _EHL_COMBOPHY_C             0x160000u   // PHY 2 — port C (EHL; also ADL-P port C combo)
#define _RKL_COMBOPHY_D             0x161000u   // PHY 3 — port D (RKL)
#define _ADL_COMBOPHY_E             0x16B000u   // PHY 4 — port E (ADL-P)

static inline uint32_t _ICL_COMBOPHY(uint8_t phy) {
    static const uint32_t bases[] = {
        _ICL_COMBOPHY_A, _ICL_COMBOPHY_B,
        _EHL_COMBOPHY_C, _RKL_COMBOPHY_D, _ADL_COMBOPHY_E,
    };
    return (phy < 5) ? bases[phy] : _ICL_COMBOPHY_A;
}

// ─── PORT_CL_DW5 ─────────────────────────────────────────────────────────────
#define _ICL_PORT_CL_DW5(phy)       (_ICL_COMBOPHY(phy) + 0x014u)
#define   SUS_CLOCK_CONFIG          (3u << 0)

// ─── PORT_PCS_DW1 ────────────────────────────────────────────────────────────
#define _ICL_PORT_PCS_AUX           0x300u
#define _ICL_PORT_PCS_GRP           0x600u
#define _ICL_PORT_PCS_DW_AUX(dw, phy)  (_ICL_COMBOPHY(phy) + _ICL_PORT_PCS_AUX + 4u*(dw))
#define _ICL_PORT_PCS_DW_GRP(dw, phy)  (_ICL_COMBOPHY(phy) + _ICL_PORT_PCS_GRP + 4u*(dw))
#define _ICL_PORT_PCS_LN(ln)            (0x800u + (ln)*0x100u)
#define _ICL_PORT_PCS_DW_LN(dw,ln,phy) (_ICL_COMBOPHY(phy) + _ICL_PORT_PCS_LN(ln) + 4u*(dw))

#define ICL_PORT_PCS_DW1_GRP(phy)   _ICL_PORT_PCS_DW_GRP(1, phy)
#define ICL_PORT_PCS_DW1_LN(ln,phy) _ICL_PORT_PCS_DW_LN(1, ln, phy)
#define   COMMON_KEEPER_EN          (1u << 26)

// ─── PORT_TX_DW offsets ──────────────────────────────────────────────────────
#define _ICL_PORT_TX_AUX            0x380u
#define _ICL_PORT_TX_GRP            0x680u
#define _ICL_PORT_TX_LN(ln)         (0x880u + (ln)*0x100u)

#define _ICL_PORT_TX_DW_AUX(dw,phy)        (_ICL_COMBOPHY(phy) + _ICL_PORT_TX_AUX + 4u*(dw))
#define _ICL_PORT_TX_DW_GRP(dw,phy)        (_ICL_COMBOPHY(phy) + _ICL_PORT_TX_GRP + 4u*(dw))
#define _ICL_PORT_TX_DW_LN(dw,ln,phy)      (_ICL_COMBOPHY(phy) + _ICL_PORT_TX_LN(ln) + 4u*(dw))

// ─── PORT_TX_DW2 — swing selection ───────────────────────────────────────────
#define ICL_PORT_TX_DW2_GRP(phy)            _ICL_PORT_TX_DW_GRP(2, phy)
#define ICL_PORT_TX_DW2_LN(ln,phy)          _ICL_PORT_TX_DW_LN(2, ln, phy)
#define   SWING_SEL_UPPER_MASK              (1u  << 15)
#define   SWING_SEL_UPPER(x)                (((x) >> 3) << 15)
#define   SWING_SEL_LOWER_MASK              (0x7u << 11)
#define   SWING_SEL_LOWER(x)                (((x) & 0x7u) << 11)
#define   RCOMP_SCALAR_MASK                 (0xFFu)
#define   RCOMP_SCALAR_VAL                  0x98u           // fixed per Linux driver

// ─── PORT_TX_DW4 — cursor / loadgen ──────────────────────────────────────────
#define ICL_PORT_TX_DW4_GRP(phy)            _ICL_PORT_TX_DW_GRP(4, phy)
#define ICL_PORT_TX_DW4_LN(ln,phy)          _ICL_PORT_TX_DW_LN(4, ln, phy)
#define   POST_CURSOR_1_MASK                (0x3Fu << 12)
#define   POST_CURSOR_1(x)                  (((x) & 0x3Fu) << 12)
#define   POST_CURSOR_2_MASK                (0x3Fu << 6)
#define   POST_CURSOR_2(x)                  (((x) & 0x3Fu) << 6)
#define   CURSOR_COEFF_MASK                 (0x3Fu)
#define   CURSOR_COEFF(x)                   ((x) & 0x3Fu)
#define   LOADGEN_SELECT                    (1u << 31)

// ─── PORT_TX_DW5 — training / scaling mode ───────────────────────────────────
#define ICL_PORT_TX_DW5_GRP(phy)            _ICL_PORT_TX_DW_GRP(5, phy)
#define ICL_PORT_TX_DW5_LN(ln,phy)          _ICL_PORT_TX_DW_LN(5, ln, phy)
#define   TX_TRAINING_EN                    (1u << 31)
#define   TAP2_DISABLE                      (1u << 30)
#define   TAP3_DISABLE                      (1u << 29)
#define   CURSOR_PROGRAM                    (1u << 26)  // REG_BIT(26)
#define   COEFF_POLARITY                    (1u << 25)  // REG_BIT(25)
#define   SCALING_MODE_SEL_MASK             (0x7u << 18)
#define   SCALING_MODE_SEL(x)               (((x) & 0x7u) << 18)
#define   RTERM_SELECT_MASK                 (0x7u << 3)
#define   RTERM_SELECT(x)                   (((x) & 0x7u) << 3)

// ─── PORT_TX_DW7 — N scalar ──────────────────────────────────────────────────
#define ICL_PORT_TX_DW7_GRP(phy)            _ICL_PORT_TX_DW_GRP(7, phy)
#define ICL_PORT_TX_DW7_LN(ln,phy)          _ICL_PORT_TX_DW_LN(7, ln, phy)
#define   N_SCALAR_MASK                     (0x7Fu << 24)
#define   N_SCALAR(x)                       (((x) & 0x7Fu) << 24)

// ─── DP Transport Control (auto-approved from mapping pipeline) ───────────────
// Canonical names: TGL_DP_TP_CTL_A / TGL_DP_TP_STATUS_A
#define TGL_DP_TP_CTL_A                     0x60540u
#define TGL_DP_TP_CTL(port)                 (TGL_DP_TP_CTL_A + ((port) * 0x100u))
#define   DP_TP_CTL_ENABLE                  (1u << 31)
#define   DP_TP_CTL_ENHANCED_FRAME_ENABLE   (1u << 18)
#define   DP_TP_CTL_LINK_TRAIN_MASK         (0x7u << 8)
#define   DP_TP_CTL_LINK_TRAIN_PAT1         (0u << 8)
#define   DP_TP_CTL_LINK_TRAIN_PAT2         (1u << 8)
#define   DP_TP_CTL_LINK_TRAIN_PAT3         (4u << 8)
#define   DP_TP_CTL_LINK_TRAIN_IDLE         (2u << 8)
#define   DP_TP_CTL_LINK_TRAIN_NORMAL       (3u << 8)

#define TGL_DP_TP_STATUS_A                  0x60544u
#define TGL_DP_TP_STATUS(port)              (TGL_DP_TP_STATUS_A + ((port) * 0x100u))

// ─── ICL PHY MISC (auto-approved from mapping pipeline) ──────────────────────
#define ICL_PHY_MISC_A                      0x64c00u
#define ICL_PHY_MISC_B                      0x64c04u
#define ICL_PHY_MISC(port)                  ((port) == 0 ? ICL_PHY_MISC_A : ICL_PHY_MISC_B)
#define   ICL_PHY_MISC_MUX_DDID             (1u << 28)
#define   ICL_PHY_MISC_DE_IO_COMP_PWR_DOWN  (1u << 23)

// ─── DDI buffer control — address base from mapping REVIEW_REQUIRED ──────────
// These addresses are HIGH-confidence from binary cross-reference.
// Stride: DDI port A = base, B = base+0x100, ...
#define DDI_BUF_CTL_A                       0x64000u
#define DDI_BUF_CTL(port)                   (DDI_BUF_CTL_A + ((port) * 0x100u))
#define   DDI_BUF_CTL_ENABLE                (1u << 31)
#define   DDI_BUF_CTL_IDLE_STATUS           (1u << 7)
#define   DDI_PORT_WIDTH_SHIFT              1u
#define   DDI_PORT_WIDTH(lanes)             (((lanes) - 1u) << DDI_PORT_WIDTH_SHIFT)

// ─── ICL DPCLKA (auto-approved from mapping pipeline) ────────────────────────
#define ICL_DPCLKA_CFGCR0                   0x164280u
#define   ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy)   (1u << ((phy) == 0 ? 10u : 11u))

// ─── TGL DDI buffer translation entry (icl union layout) ─────────────────────
// Matches: union intel_ddi_buf_trans_entry .icl member in Linux
// Fields:  [0] iboost  [1] dw2_swing_sel  [2] dw7_n_scalar
//          [3] dw4_post_cursor_1          [4] dw4_cursor_coeff (also used as n_scalar high)
struct TGLComboBufTransEntry {
    uint8_t iboost;             // unused in this impl (hw iboost controlled separately)
    uint8_t dw2_swing_sel;      // → SWING_SEL_UPPER/LOWER in DW2
    uint8_t dw7_n_scalar;       // → N_SCALAR in DW7
    uint8_t dw4_post_cursor_1;  // → POST_CURSOR_1 in DW4
    uint8_t dw4_cursor_coeff;   // → CURSOR_COEFF in DW4
};

// Level index from (voltageSwing, preEmphasis) — DP spec table, same ordering Linux uses
// swing 0..3, pre 0..3 (only valid combos per spec: pre <= 3-swing)
static inline int tgl_combo_phy_trans_index(uint8_t swing, uint8_t pre) {
    // DP spec defines 10 valid combinations:
    // swing=0: pre 0,1,2,3  → levels 0,1,2,3
    // swing=1: pre 0,1,2    → levels 4,5,6
    // swing=2: pre 0,1      → levels 7,8
    // swing=3: pre 0        → level  9
    static const int8_t table[4][4] = {
        { 0,  1,  2,  3},   // swing 0
        { 4,  5,  6, -1},   // swing 1
        { 7,  8, -1, -1},   // swing 2
        { 9, -1, -1, -1},   // swing 3
    };
    if (swing > 3 || pre > 3) return 0;
    int idx = table[swing][pre];
    return (idx < 0) ? 0 : idx;
}

// ─── Actual translation tables from Linux intel_ddi_buf_trans.c ──────────────
// tgl_combo_phy_trans_dp_hbr  (RBR / HBR — ≤2.7 Gbps)
static const TGLComboBufTransEntry tgl_combo_phy_trans_dp_hbr[10] = {
    // iboost dw2_swing dw7_n  dw4_pc1 dw4_cc
    { 0xA, 0x32, 0x3F, 0x00, 0x00 }, // L0: 350/350  0.0 dB
    { 0xA, 0x4F, 0x37, 0x00, 0x08 }, // L1: 350/500  3.1 dB
    { 0xC, 0x71, 0x2F, 0x00, 0x10 }, // L2: 350/700  6.0 dB
    { 0x6, 0x7D, 0x2B, 0x00, 0x14 }, // L3: 350/900  8.2 dB
    { 0xA, 0x4C, 0x3F, 0x00, 0x00 }, // L4: 500/500  0.0 dB
    { 0xC, 0x73, 0x34, 0x00, 0x0B }, // L5: 500/700  2.9 dB
    { 0x6, 0x7F, 0x2F, 0x00, 0x10 }, // L6: 500/900  5.1 dB
    { 0xC, 0x6C, 0x3C, 0x00, 0x03 }, // L7: 650/700  0.6 dB
    { 0x6, 0x7F, 0x35, 0x00, 0x0A }, // L8: 600/900  3.5 dB
    { 0x6, 0x7F, 0x3F, 0x00, 0x00 }, // L9: 900/900  0.0 dB
};

// tgl_combo_phy_trans_dp_hbr2  (HBR2 / HBR3 — 5.4 / 8.1 Gbps)
static const TGLComboBufTransEntry tgl_combo_phy_trans_dp_hbr2[10] = {
    { 0xA, 0x35, 0x3F, 0x00, 0x00 },
    { 0xA, 0x4F, 0x37, 0x00, 0x08 },
    { 0xC, 0x63, 0x2F, 0x00, 0x10 },
    { 0x6, 0x7F, 0x2B, 0x00, 0x14 },
    { 0xA, 0x47, 0x3F, 0x00, 0x00 },
    { 0xC, 0x63, 0x34, 0x00, 0x0B },
    { 0x6, 0x7F, 0x2F, 0x00, 0x10 },
    { 0xC, 0x61, 0x3C, 0x00, 0x03 },
    { 0x6, 0x7B, 0x35, 0x00, 0x0A },
    { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};

// ─── ADL-P combo PHY translation tables (from Linux intel_ddi_buf_trans.c) ───
// ADL-P combo PHY uses icl_combo_phy_set_signal_levels() — same register sequence as TGL —
// but with different table values.  Only ports A and B are combo PHY on ADL-P;
// ports C-F use DKL PHY (not implemented here).

// adlp_combo_phy_trans_dp_hbr  (RBR / HBR — ≤2.7 Gbps)
static const TGLComboBufTransEntry adlp_combo_phy_trans_dp_hbr[10] = {
    // iboost dw2_swing dw7_n  dw4_pc1 dw4_cc
    { 0xA, 0x35, 0x3F, 0x00, 0x00 }, // L0: 350/350  0.0 dB
    { 0xA, 0x4F, 0x37, 0x00, 0x08 }, // L1: 350/500  3.1 dB
    { 0xC, 0x71, 0x31, 0x00, 0x0E }, // L2: 350/700  6.0 dB
    { 0x6, 0x7F, 0x2C, 0x00, 0x13 }, // L3: 350/900  8.2 dB
    { 0xA, 0x4C, 0x3F, 0x00, 0x00 }, // L4: 500/500  0.0 dB
    { 0xC, 0x73, 0x34, 0x00, 0x0B }, // L5: 500/700  2.9 dB
    { 0x6, 0x7F, 0x2F, 0x00, 0x10 }, // L6: 500/900  5.1 dB
    { 0xC, 0x7C, 0x3C, 0x00, 0x03 }, // L7: 650/700  0.6 dB
    { 0x6, 0x7F, 0x35, 0x00, 0x0A }, // L8: 600/900  3.5 dB
    { 0x6, 0x7F, 0x3F, 0x00, 0x00 }, // L9: 900/900  0.0 dB
};

// adlp_combo_phy_trans_dp_hbr2_hbr3  (HBR2 / HBR3 — 5.4 / 8.1 Gbps)
static const TGLComboBufTransEntry adlp_combo_phy_trans_dp_hbr2[10] = {
    { 0xA, 0x35, 0x3F, 0x00, 0x00 }, // L0: 350/350  0.0 dB
    { 0xA, 0x4F, 0x37, 0x00, 0x08 }, // L1: 350/500  3.1 dB
    { 0xC, 0x71, 0x30, 0x00, 0x0F }, // L2: 350/700  6.0 dB
    { 0x6, 0x7F, 0x2B, 0x00, 0x14 }, // L3: 350/900  8.2 dB
    { 0xA, 0x4C, 0x3F, 0x00, 0x00 }, // L4: 500/500  0.0 dB
    { 0xC, 0x73, 0x34, 0x00, 0x0B }, // L5: 500/700  2.9 dB
    { 0x6, 0x7F, 0x30, 0x00, 0x0F }, // L6: 500/900  5.1 dB
    { 0xC, 0x63, 0x3F, 0x00, 0x00 }, // L7: 650/700  0.6 dB
    { 0x6, 0x7F, 0x38, 0x00, 0x07 }, // L8: 600/900  3.5 dB
    { 0x6, 0x7F, 0x3F, 0x00, 0x00 }, // L9: 900/900  0.0 dB
};

// adlp_combo_phy_trans_edp_up_to_hbr2  (eDP ≤HBR2 — lower swing, finer steps)
static const TGLComboBufTransEntry adlp_combo_phy_trans_edp_hbr2[10] = {
    { 0x4, 0x50, 0x38, 0x00, 0x07 }, // 200/200  0.0 dB
    { 0x4, 0x58, 0x35, 0x00, 0x0A }, // 200/250  1.9 dB
    { 0x4, 0x60, 0x34, 0x00, 0x0B }, // 200/300  3.5 dB
    { 0x4, 0x6A, 0x32, 0x00, 0x0D }, // 200/350  4.9 dB
    { 0x4, 0x5E, 0x38, 0x00, 0x07 }, // 250/250  0.0 dB
    { 0x4, 0x61, 0x36, 0x00, 0x09 }, // 250/300  1.6 dB
    { 0x4, 0x6B, 0x34, 0x00, 0x0B }, // 250/350  2.9 dB
    { 0x4, 0x69, 0x39, 0x00, 0x06 }, // 300/300  0.0 dB
    { 0x4, 0x73, 0x37, 0x00, 0x08 }, // 300/350  1.3 dB
    { 0x4, 0x7A, 0x38, 0x00, 0x07 }, // 350/350  0.0 dB
};

// ─── Public API ──────────────────────────────────────────────────────────────

namespace IntelDPLinkTraining {

// TGL / ICL combo PHY — uses tgl_combo_phy_trans_dp_hbr / hbr2 tables.
void setSignalLevels(
    uint8_t phy,
    uint8_t laneCount,
    bool    isHBR2,
    bool    isDP,
    const uint8_t voltageSwing[4],
    const uint8_t preEmphasis[4]
);

// ADL-P combo PHY — same register sequence as TGL but uses adlp_combo_phy_trans_dp_* tables.
// Only PHY 0 (port A, eDP) and PHY 1 (port B, DP) are combo PHY on ADL-P.
// isEDP: selects adlp_combo_phy_trans_edp_hbr2 (true, ≤HBR2) vs dp_hbr/hbr2 tables (false).
void setSignalLevelsADLP(
    uint8_t phy,
    uint8_t laneCount,
    bool    isHBR2,
    bool    isEDP,
    const uint8_t voltageSwing[4],
    const uint8_t preEmphasis[4]
);

// Thin wrappers for the reference-repo calling convention (one call per
// setVoltageSwing / setPreEmphasis invocation on a given PHY).
// These can be called instead of setSignalLevels when you need to preserve
// the original two-step call pattern.
void programVoltageSwing(uint8_t phy, bool isHBR2, uint8_t laneCount,
                         const uint8_t swing[4], const uint8_t preEmph[4]);
void programPreEmphasis (uint8_t phy, bool isHBR2, uint8_t laneCount,
                         const uint8_t swing[4], const uint8_t preEmph[4]);

} // namespace IntelDPLinkTraining
