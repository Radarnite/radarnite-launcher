// The cosmetics panel's control ids and timers, shared by ui.cpp (which routes input and timers to the panel) and
// port_ui.cpp (which draws it). They sit above every id of ui.cpp so the two never collide.
#pragma once

namespace rn {

enum : int {
    ID_PORT = 5000,       // everything >= ID_PORT and < ID_P_END belongs to the panel (activate() hands it over)
    ID_P_OPEN = ID_PORT, ID_P_REFRESH, ID_P_CLOSE, ID_P_CAT, ID_P_FILTERS, ID_P_NEW, ID_P_SEARCH, ID_P_SORT,
    ID_P_CLEARSEL, ID_P_IMPORTSEL, ID_P_IMPORTALL, ID_P_STOP, ID_P_LOG, ID_P_GRID, ID_P_SB, ID_P_SCRIM, ID_P_POP,
    ID_P_CLEARFILTERS, ID_P_DONE, ID_P_DLG, ID_P_DLG_CANCEL, ID_P_DLG_IMPORT, ID_P_PV, ID_P_PV_CLOSE, ID_P_PV_SELECT,
    ID_P_PV_IMPORT, ID_P_PV_LINK, ID_P_PV_MEDIA, ID_P_RETRY,
    ID_P_CHIP0 = 5200,    // popup chips: categories 5200.., status 5220.., types 5240.., rarities 5260.., sort 5300..
    ID_P_CARD0 = 6000,    // visible card slots (never item indices: animation keys and focus stay small)
    ID_P_QB0 = 6600,      // their select buttons
    ID_P_END = 7200,
};
const int P_SLOTS = 600;
const UINT_PTR T_PORT = 4;    // 1 s while the panel shows a running import or a load (elapsed times)

} // namespace rn
