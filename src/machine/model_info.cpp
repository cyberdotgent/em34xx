// SPDX-License-Identifier: GPL-3.0-or-later
#include "machine/model_info.hpp"

namespace em34xx {

const char* model_status_name(ModelStatus s) {
    switch (s) {
    case ModelStatus::Bringup: return "bring-up";
    case ModelStatus::RomIncomplete: return "ROM incomplete";
    case ModelStatus::Placeholder: return "placeholder";
    }
    return "?";
}

} // namespace em34xx
