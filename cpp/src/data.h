#pragma once

#include "common.h"

namespace fc {

void convert_structured(const Job& job, EventSink& sink);
void convert_table_to_json(const Job& job, EventSink& sink);

}  // namespace fc