#include "fc/core.h"

#include <cstdio>
#include <cstring>

int main() {
    const char* version = fc_version();
    if (!version || !std::strstr(version, "1.6.0")) {
        std::fprintf(stderr, "unexpected version: %s\n", version ? version : "(null)");
        return 1;
    }
    const char* registry = fc_registry_json();
    if (!registry || !std::strstr(registry, "image_convert") ||
        !std::strstr(registry, "pdf_to_image") || !std::strstr(registry, "table_to_json")) {
        std::fprintf(stderr, "registry is missing expected conversion ids\n");
        return 2;
    }
    fc_context* context = fc_context_create(nullptr);
    if (!context) {
        std::fprintf(stderr, "fc_context_create failed\n");
        return 3;
    }
    fc_context_destroy(context);
    std::puts("fc core smoke test ok");
    return 0;
}