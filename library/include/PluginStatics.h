/*
 * This file and the companion PluginStatics.cpp contain static structures used
 * by DFHack plugins. Linking them here, into the dfhack library, instead of
 * into the plugins themselves allows the plugins to be freely unloaded and
 * reloaded without fear of causing cached references to static data becoming
 * corrupted.
 */

#pragma once

#include <xlsxio_read.h>

#include "DataIdentity.h"

namespace DFHack {
    struct xlsx_file_handle_identity;
    struct xlsx_sheet_handle_identity;

// xlsxreader definitions
    struct DFHACK_EXPORT xlsx_file_handle {
    const xlsxioreader handle;
    const xlsx_file_handle(xlsxioreader handle): handle(handle) {}
    static const xlsx_file_handle_identity _identity;
};

struct DFHACK_EXPORT xlsx_sheet_handle {
    const xlsxioreadersheet handle;
    const xlsx_sheet_handle(xlsxioreadersheet handle): handle(handle) {}
    static const xlsx_sheet_handle_identity _identity;
};

struct DFHACK_EXPORT xlsx_file_handle_identity : public compound_identity_base {
    xlsx_file_handle_identity()
        :compound_identity_base(0, typeid(xlsx_file_handle), nullptr, nullptr, "xlsx_file_handle") {};
    DFHack::identity_type type() const override { return IDTYPE_OPAQUE; }
};

struct DFHACK_EXPORT xlsx_sheet_handle_identity : public compound_identity_base {
    xlsx_sheet_handle_identity()
        :compound_identity_base(0, typeid(xlsx_sheet_handle), nullptr, nullptr, "xlsx_sheet_handle") {};
    DFHack::identity_type type() const override { return IDTYPE_OPAQUE; }
};


}
