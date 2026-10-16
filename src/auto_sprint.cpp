#include <bedrock/mod.hpp>

// That's all a client needs to write: SDK handles boot, manifest, input and stop.
BEDROCK_MOD([](bedrock::Client& client) {
    client.input().autoSprint();
});
