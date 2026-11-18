// SPDX-License-Identifier: MIT
// Which stand-in a texture's draws sample and when it is current within a range (RawMetalRangeStep.mm standIns). A view
// of an extended-range or RG8Unorm_sRGB texture in the same format samples a view of its texture's stand-in, the same
// memory; one in another transfer of the same codes samples a stand-in of its own (raw_texture_views.cpp
// viewCompanions). Every one of them holds what the codes of one storage owner (the view's levels owner) read, so a write
// through any texture over those codes makes all of them stale.
#pragma once
#import "RawMetalInternal.h"

#include <algorithm>
#include <utility>
#include <vector>

// The texture whose memory `t`'s texels are
inline RMTexture *rmStorageOwner(RMTexture *t)
{
    return t->_levelsOwner ?: t;
}

// The texture whose conversion makes `t`'s stand-in current: its storage owner where `t` reads the owner's transfer (its
// stand-in a view of the owner's), else `t`
inline RMTexture *rmStandInHolder(RMTexture *t)
{
    RMTexture *o = rmStorageOwner(t);
    return o != t && o->_raw.standIn && t->_raw.format == o->_raw.format ? o : t;
}

// Whether a write to `t` changes codes some stand-in is drawn from
inline bool rmStandInBacked(RMTexture *t)
{
    return t->_raw.standIn || rmStorageOwner(t)->_raw.standIn;
}

// What the conversion of `holder`'s stand-in reads and draws into: its storage owner's codes, of `holder`'s format, and the
// stand-in `holder` owns (a view's own stand-in is the one its header object holds)
inline raw::Texture rmStandInCodes(RMTexture *holder)
{
    RMTexture *o = rmStorageOwner(holder);
    if (o == holder)
        return holder->_raw;
    raw::Texture codes = o->_raw;
    codes.format = holder->_raw.format;
    codes.standIn = holder->_raw.standIn->standIn;
    return codes;
}

// The stand-ins drawn current in a range: one record a (storage owner, holder) pair, so a view and its texture share one
// where they share a stand-in, and a write through either drops every record of their codes
class RMStandInFresh {
public:
    bool insert(RMTexture *t)
    {
        if (!t || count(t))
            return false;
        held_.push_back({rmStorageOwner(t), t});
        return true;
    }

    size_t count(RMTexture *t) const
    {
        return t && std::find(held_.begin(), held_.end(), std::make_pair(rmStorageOwner(t), t)) != held_.end();
    }

    void erase(RMTexture *t)
    {
        if (!t)
            return;
        RMTexture *o = rmStorageOwner(t);
        held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const auto &r) { return r.first == o; }),
                    held_.end());
    }

    void clear()
    {
        held_.clear();
    }

    bool empty() const
    {
        return held_.empty();
    }

private:
    std::vector<std::pair<RMTexture *, RMTexture *>> held_;
};
