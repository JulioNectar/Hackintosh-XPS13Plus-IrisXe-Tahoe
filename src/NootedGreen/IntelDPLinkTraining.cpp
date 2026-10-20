// SPDX-License-Identifier: MIT
//
// IntelDPLinkTraining.cpp — Tiger Lake combo PHY DP voltage swing & pre-emphasis
//
// Implements the register-write sequence from Linux:
//   icl_combo_phy_set_signal_levels()  → drivers/gpu/drm/i915/display/intel_ddi.c
//   icl_ddi_combo_vswing_program()     → ibid.
//
// Only combo PHY A (eDP) and B (DP-B) are supported.
// TC/MG PHY (ports C-F) require tgl_dkl_phy_set_signal_levels() — not implemented here.
//
// NootedGreen register access:
//   NGreen::callback->readReg32(addr)
//   NGreen::callback->writeReg32(addr, val)
//   NGreen::callback->intel_de_rmw(addr, clear_mask, set_bits)

#include "IntelDPLinkTraining.hpp"
#include "kern_green.hpp"          // NGreen::callback, readReg32/writeReg32/intel_de_rmw
#include <Headers/kern_util.hpp>   // SYSLOG

// ─── helpers ─────────────────────────────────────────────────────────────────

static uint32_t ng_read(uint32_t reg) {
    return NGreen::callback->readReg32(reg);
}
static void ng_write(uint32_t reg, uint32_t val) {
    NGreen::callback->writeReg32(reg, val);
}
static void ng_rmw(uint32_t reg, uint32_t clear, uint32_t set) {
    NGreen::callback->intel_de_rmw(reg, clear, set);
}

// ─── icl_combo_phy_loadgen_select equivalent ─────────────────────────────────
// Linux rules (for DP, rates <= 6 GHz):
//   4 lanes:  LN0=0, LN1=1, LN2=1, LN3=1
//   1/2 lanes: LN0=0, LN1=1, LN2=1, LN3=0
//   > 6 GHz (HBR3): all = 0
static uint32_t loadgen_for_lane(uint8_t lane, uint8_t laneCount, bool isHBR3) {
    if (isHBR3) return 0;
    // lane 0 always 0
    if (lane == 0) return 0;
    // lane 3 only set for 4-lane
    if (lane == 3) return (laneCount == 4) ? LOADGEN_SELECT : 0;
    // lanes 1,2 always set for 2+ lane DP
    return LOADGEN_SELECT;
}

// ─── icl_ddi_combo_vswing_program equivalent ─────────────────────────────────
static void combo_vswing_program(
    uint8_t phy,
    uint8_t laneCount,
    const TGLComboBufTransEntry *table,
    const uint8_t swing[4],
    const uint8_t preEmph[4])
{
    // Step 5a: program DW5 GRP — scaling mode + tap3 disable, clear TX_TRAINING_EN
    // (TX_TRAINING_EN already cleared before calling here; just set the rest)
    {
        uint32_t dw5 = ng_read(ICL_PORT_TX_DW5_LN(0, phy));
        dw5 &= ~(SCALING_MODE_SEL_MASK | RTERM_SELECT_MASK |
                 COEFF_POLARITY | CURSOR_PROGRAM | TAP2_DISABLE | TAP3_DISABLE);
        dw5 |= SCALING_MODE_SEL(0x2u);
        dw5 |= RTERM_SELECT(0x6u);
        dw5 |= TAP3_DISABLE;
        ng_write(ICL_PORT_TX_DW5_GRP(phy), dw5);
    }

    // Per-lane DW2, DW4, DW7
    for (uint8_t ln = 0; ln < 4; ln++) {
        // Lanes beyond laneCount still need to be programmed (Linux does all 4)
        int level = tgl_combo_phy_trans_index(swing[ln], preEmph[ln]);
        const TGLComboBufTransEntry *e = &table[level];

        // DW2: swing selection + RCOMP scalar
        ng_rmw(ICL_PORT_TX_DW2_LN(ln, phy),
               SWING_SEL_UPPER_MASK | SWING_SEL_LOWER_MASK | RCOMP_SCALAR_MASK,
               SWING_SEL_UPPER(e->dw2_swing_sel) |
               SWING_SEL_LOWER(e->dw2_swing_sel) |
               RCOMP_SCALAR_VAL);

        // DW4: cursor coefficients (note: post_cursor_1=0 for all TGL entries,
        // but we still use the struct field so the code is table-driven)
        ng_rmw(ICL_PORT_TX_DW4_LN(ln, phy),
               POST_CURSOR_1_MASK | POST_CURSOR_2_MASK | CURSOR_COEFF_MASK,
               POST_CURSOR_1(e->dw4_post_cursor_1) |
               POST_CURSOR_2(0u) |            // TGL entries have dw4_post_cursor_2=0
               CURSOR_COEFF(e->dw4_cursor_coeff));

        // DW7: N scalar
        ng_rmw(ICL_PORT_TX_DW7_LN(ln, phy),
               N_SCALAR_MASK,
               N_SCALAR(e->dw7_n_scalar));
    }
}

// ─── Public API implementation ────────────────────────────────────────────────

void IntelDPLinkTraining::setSignalLevels(
    uint8_t phy,
    uint8_t laneCount,
    bool    isHBR2,
    bool    isDP,
    const uint8_t voltageSwing[4],
    const uint8_t preEmphasis[4])
{
    if (!NGreen::callback) {
        SYSLOG("ngreen", "IntelDPLinkTraining: callback is null");
        return;
    }
    if (phy > 4) {
        SYSLOG("ngreen", "IntelDPLinkTraining: PHY %u out of range (0=A,1=B,2=C,3=D,4=E)", phy);
        return;
    }

    const TGLComboBufTransEntry *table = isHBR2
        ? tgl_combo_phy_trans_dp_hbr2
        : tgl_combo_phy_trans_dp_hbr;

    bool isHBR3 = false; // HBR3 (8.1 Gbps): loadgen all-zero. Caller can extend.

    // Step 1: set/clear COMMON_KEEPER_EN in PCS_DW1
    {
        uint32_t val = ng_read(ICL_PORT_PCS_DW1_LN(0, phy));
        if (isDP)
            val |= COMMON_KEEPER_EN;
        else
            val &= ~COMMON_KEEPER_EN;
        ng_write(ICL_PORT_PCS_DW1_GRP(phy), val);
    }

    // Step 2: program LOADGEN_SELECT per lane
    for (uint8_t ln = 0; ln < 4; ln++) {
        ng_rmw(ICL_PORT_TX_DW4_LN(ln, phy),
               LOADGEN_SELECT,
               loadgen_for_lane(ln, laneCount, isHBR3));
    }

    // Step 3: set SUS_CLOCK_CONFIG in CL_DW5
    ng_rmw(_ICL_PORT_CL_DW5(phy), 0, SUS_CLOCK_CONFIG);

    // Step 4: clear TX_TRAINING_EN so we can safely change swing values
    {
        uint32_t val = ng_read(ICL_PORT_TX_DW5_LN(0, phy));
        val &= ~TX_TRAINING_EN;
        ng_write(ICL_PORT_TX_DW5_GRP(phy), val);
    }

    // Step 5: program swing + de-emphasis (DW2, DW4, DW7 per lane; DW5 GRP for mode)
    combo_vswing_program(phy, laneCount, table, voltageSwing, preEmphasis);

    // Step 6: set TX_TRAINING_EN to latch the new values
    {
        uint32_t val = ng_read(ICL_PORT_TX_DW5_LN(0, phy));
        val |= TX_TRAINING_EN;
        ng_write(ICL_PORT_TX_DW5_GRP(phy), val);
    }

    SYSLOG("ngreen", "IntelDPLinkTraining: PHY %u programmed (HBR2=%d lanes=%u)", phy, isHBR2, laneCount);
}

// ─── ADL-P variant ───────────────────────────────────────────────────────────
// Same 6-step register sequence as TGL (icl_combo_phy_set_signal_levels),
// but uses ADL-P specific translation tables from Linux adlp_get_combo_buf_trans.
// Platform rule (from adlp_get_combo_buf_trans):
//   eDP + HBR2  → adlp_combo_phy_trans_edp_hbr2
//   eDP + HBR3  → adlp_combo_phy_trans_dp_hbr2  (shared HBR2/HBR3 table for eDP HBR3)
//   DP  + HBR2+ → adlp_combo_phy_trans_dp_hbr2
//   DP  + HBR   → adlp_combo_phy_trans_dp_hbr
void IntelDPLinkTraining::setSignalLevelsADLP(
    uint8_t phy,
    uint8_t laneCount,
    bool    isHBR2,
    bool    isEDP,
    const uint8_t voltageSwing[4],
    const uint8_t preEmphasis[4])
{
    if (!NGreen::callback) {
        SYSLOG("ngreen", "IntelDPLinkTraining ADLP: callback is null");
        return;
    }
    // ADL-P combo PHY: only ports A (0) and B (1) — TC ports C-F are DKL PHY
    if (phy > 1) {
        SYSLOG("ngreen", "IntelDPLinkTraining ADLP: PHY %u is not a combo PHY on ADL-P", phy);
        return;
    }

    const TGLComboBufTransEntry *table;
    if (isEDP) {
        // eDP: always use the eDP-specific low-swing table (≤HBR2).
        // Linux uses adlp_combo_phy_trans_edp_hbr3 for HBR3 eDP, which shares the
        // dp_hbr2 data — handled by the else branch below for simplicity.
        table = isHBR2 ? adlp_combo_phy_trans_dp_hbr2 : adlp_combo_phy_trans_edp_hbr2;
    } else {
        table = isHBR2 ? adlp_combo_phy_trans_dp_hbr2 : adlp_combo_phy_trans_dp_hbr;
    }

    bool isDP    = true;  // combo PHY on ADL-P is always DP or eDP
    bool isHBR3  = false;

    // Step 1: COMMON_KEEPER_EN in PCS_DW1
    {
        uint32_t val = ng_read(ICL_PORT_PCS_DW1_LN(0, phy));
        val |= COMMON_KEEPER_EN;  // always DP/eDP on ADL-P combo PHY
        ng_write(ICL_PORT_PCS_DW1_GRP(phy), val);
    }
    // Step 2: LOADGEN_SELECT per lane
    for (uint8_t ln = 0; ln < 4; ln++)
        ng_rmw(ICL_PORT_TX_DW4_LN(ln, phy), LOADGEN_SELECT, loadgen_for_lane(ln, laneCount, isHBR3));
    // Step 3: SUS_CLOCK_CONFIG
    ng_rmw(_ICL_PORT_CL_DW5(phy), 0, SUS_CLOCK_CONFIG);
    // Step 4: clear TX_TRAINING_EN
    {
        uint32_t val = ng_read(ICL_PORT_TX_DW5_LN(0, phy));
        val &= ~TX_TRAINING_EN;
        ng_write(ICL_PORT_TX_DW5_GRP(phy), val);
    }
    // Step 5: program swing + de-emphasis
    combo_vswing_program(phy, laneCount, table, voltageSwing, preEmphasis);
    // Step 6: set TX_TRAINING_EN to latch
    {
        uint32_t val = ng_read(ICL_PORT_TX_DW5_LN(0, phy));
        val |= TX_TRAINING_EN;
        ng_write(ICL_PORT_TX_DW5_GRP(phy), val);
    }

    SYSLOG("ngreen", "IntelDPLinkTraining ADLP: PHY %u programmed (HBR2=%d eDP=%d lanes=%u)",
           phy, isHBR2, isEDP, laneCount);
}

// Convenience wrappers — thin delegates to setSignalLevels
void IntelDPLinkTraining::programVoltageSwing(
    uint8_t phy, bool isHBR2, uint8_t laneCount,
    const uint8_t swing[4], const uint8_t preEmph[4])
{
    setSignalLevels(phy, laneCount, isHBR2, /*isDP=*/true, swing, preEmph);
}

void IntelDPLinkTraining::programPreEmphasis(
    uint8_t phy, bool isHBR2, uint8_t laneCount,
    const uint8_t swing[4], const uint8_t preEmph[4])
{
    // pre-emphasis cannot be set independently of swing in the combo PHY —
    // both are encoded in the same table entry and written together.
    // This wrapper exists for call-site symmetry with the reference repo.
    setSignalLevels(phy, laneCount, isHBR2, /*isDP=*/true, swing, preEmph);
}
