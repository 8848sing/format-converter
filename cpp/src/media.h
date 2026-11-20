#pragma once

#include "common.h"

namespace fc {

void convert_media(const Job& job, EventSink& sink);
fs::path find_ffmpeg(const Job& job);

}  // namespace fc