//  Copyright © 2022-2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

// Not final, will be re-done eventually.

#ifndef kern_model_hpp
#define kern_model_hpp
#include "kern_green.hpp"
#include <Headers/kern_util.hpp>

struct DeviceList {
    uint16_t dev {0};
    const char *name {nullptr};
};

struct DevicePair {
    const DeviceList *models;
    size_t modelNum;
};

static constexpr DeviceList g7[] = {
    {0x0402, "Intel HD Graphics (Haswell)"},
    {0x0406, "Intel HD Graphics (Haswell)"},
    {0x040A, "Intel HD Graphics (Haswell)"},
    {0x040B, "Intel HD Graphics (Haswell)"},
    {0x040E, "Intel HD Graphics (Haswell)"},
    {0x041E, "Intel HD 4400 Graphics"},
    {0x0A02, "Intel HD Graphics (Haswell ULT)"},
    {0x0A06, "Intel HD Graphics (Haswell ULT)"},
    {0x0A0A, "Intel HD Graphics (Haswell ULT)"},
    {0x0A0B, "Intel HD Graphics (Haswell ULT)"},
    {0x0A0E, "Intel HD Graphics (Haswell ULT)"},
    {0x0A16, "Intel HD 4400 Graphics"},
};

static constexpr DeviceList g8[] = {
    // TBF
    {0x1606, "Intel HD Graphics (Broadwell)"},
    {0x22B0, "Intel HD Graphics 40X"},
    {0x22B1, "Intel HD Graphics 40X"},
    {0x22B2, "Intel HD Graphics 40X"},
    {0x22B3, "Intel HD Graphics 400"},
};

static constexpr DeviceList g9_5[] = {
    // TBF
    {0x3184, "Intel UHD Graphics 605"},
    {0x3185, "Intel UHD Graphics 600"},
};

// Gen11 — Ice Lake
static constexpr DeviceList g11_icl[] = {
    {0x8A51, "Intel Iris Plus Graphics G7"},
    {0x8A52, "Intel Iris Plus Graphics G7"},
    {0x8A53, "Intel Iris Plus Graphics G7"},
    {0x8A56, "Intel Iris Plus Graphics G1"},
    {0x8A58, "Intel UHD Graphics G1"},
    {0x8A5A, "Intel Iris Plus Graphics G4"},
    {0x8A5C, "Intel Iris Plus Graphics G4"},
};

// Gen11 — Jasper Lake
static constexpr DeviceList g11_jsl[] = {
    {0x4E55, "Intel UHD Graphics"},
    {0x4E61, "Intel UHD Graphics"},
    {0x4E71, "Intel UHD Graphics"},
};

// Gen11 — Elkhart Lake
static constexpr DeviceList g11_ehl[] = {
    {0x4555, "Intel UHD Graphics"},
    {0x4571, "Intel UHD Graphics"},
};

// Gen12 — Tiger Lake
static constexpr DeviceList g12_tgl[] = {
    {0x9A40, "Intel Iris Xe Graphics"},
    {0x9A49, "Intel Iris Xe Graphics"},
    {0x9A60, "Intel UHD Graphics"},
    {0x9A68, "Intel UHD Graphics"},
    {0x9A70, "Intel UHD Graphics"},
    {0x9A78, "Intel UHD Graphics"},
};

// Gen12 — Rocket Lake
static constexpr DeviceList g12_rkl[] = {
    {0x4C8A, "Intel UHD Graphics 750"},
    {0x4C8B, "Intel UHD Graphics 730"},
    {0x4C90, "Intel UHD Graphics P750"},
    {0x4C9A, "Intel UHD Graphics"},
};

// Gen12 — Alder Lake
static constexpr DeviceList g12_adl[] = {
    {0x4680, "Intel UHD Graphics 770"},
    {0x4682, "Intel UHD Graphics 730"},
    {0x4688, "Intel UHD Graphics 770"},
    {0x468A, "Intel UHD Graphics"},
    {0x4690, "Intel UHD Graphics 770"},
    {0x4692, "Intel UHD Graphics 730"},
    {0x4693, "Intel UHD Graphics 710"},
    {0x4626, "Intel Iris Xe Graphics"},
    {0x4628, "Intel UHD Graphics"},
    {0x462A, "Intel UHD Graphics"},
    {0x46A3, "Intel UHD Graphics"},
    {0x46A6, "Intel Iris Xe Graphics"},
    {0x46A8, "Intel Iris Xe Graphics"},
    {0x46AA, "Intel Iris Xe Graphics"},
    {0x46B3, "Intel UHD Graphics"},
    {0x46C3, "Intel UHD Graphics"},
    {0x46D0, "Intel UHD Graphics"},
    {0x46D1, "Intel UHD Graphics"},
    {0x46D2, "Intel UHD Graphics"},
};

// Gen12 — Raptor Lake
static constexpr DeviceList g12_rpl[] = {
    {0xA780, "Intel UHD Graphics 770"},
    {0xA781, "Intel UHD Graphics"},
    {0xA782, "Intel UHD Graphics"},
    {0xA783, "Intel UHD Graphics"},
    {0xA788, "Intel UHD Graphics"},
    {0xA789, "Intel UHD Graphics"},
    {0xA78A, "Intel UHD Graphics"},
    {0xA78B, "Intel UHD Graphics"},
    {0xA7A0, "Intel Iris Xe Graphics"},
    {0xA7A1, "Intel Iris Xe Graphics"},
    {0xA7A8, "Intel UHD Graphics"},
    {0xA7A9, "Intel UHD Graphics"},
    {0xA720, "Intel UHD Graphics"},
    {0xA721, "Intel UHD Graphics"},
};

static constexpr DevicePair devices[] = {
    {g7, arrsize(g7)},
    {g8, arrsize(g8)},
    {g9_5, arrsize(g9_5)},
    {g11_icl, arrsize(g11_icl)},
    {g11_jsl, arrsize(g11_jsl)},
    {g11_ehl, arrsize(g11_ehl)},
    {g12_tgl, arrsize(g12_tgl)},
    {g12_rkl, arrsize(g12_rkl)},
    {g12_adl, arrsize(g12_adl)},
    {g12_rpl, arrsize(g12_rpl)},
};

inline const char *getBranding(uint16_t dev) {
    for (auto &device : devices) {
        for (size_t i = 0; i < device.modelNum; i++) {
            auto &model = device.models[i];
            if (model.dev == dev) { return model.name; }
        }
    }

    return "Intel HD/UHD Graphics";
}

#endif /* kern_model_hpp */
