// deadpool-decomp: see libavcodec/avcodec.h in this folder. GPL-3.0-or-later (see COPYING).
#pragma once
#include "../libavcodec/avcodec.h"
inline void av_channel_layout_default(AVChannelLayout* l, int n) { l->nb_channels = n; }
