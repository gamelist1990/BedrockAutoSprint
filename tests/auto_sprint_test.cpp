#include <bedrock/events.hpp>
#include "auto_sprint_feature.hpp"
#include <cassert>
#include <vector>

int main() {
    bedrock::ClientEvents events;
    std::vector<bool> keyTransitions;
    bool allowed = true;
    {
        AutoSprintFeature feature(events,
            [&](bool press) { keyTransitions.push_back(press); return true; },
            [&] { return allowed; });
        assert(events.listeners<bedrock::UpdateEvent>() == 1);

        events.emit(bedrock::KeyEvent{'W',true,false});
        events.emit(bedrock::UpdateEvent{1});
        assert(keyTransitions.empty());  // Must be in-game.

        events.emit(bedrock::GameplayEvent{true});
        events.emit(bedrock::UpdateEvent{2});
        assert((keyTransitions == std::vector<bool>{true}));

        events.emit(bedrock::UpdateEvent{3});
        assert(keyTransitions.size() == 1); // No repeat injections.

        events.emit(bedrock::KeyEvent{0xA0,true,false});
        events.emit(bedrock::UpdateEvent{4});
        assert((keyTransitions == std::vector<bool>{true,false}));

        allowed = false;
        events.emit(bedrock::KeyEvent{0xA0,false,false});
        events.emit(bedrock::UpdateEvent{5});
        assert(keyTransitions.size() == 2); // Respect user-held Ctrl.

        allowed = true;
        events.emit(bedrock::UpdateEvent{6});
        assert((keyTransitions == std::vector<bool>{true,false,true}));

        events.emit(bedrock::GameplayEvent{false});
        events.emit(bedrock::UpdateEvent{7});
        assert((keyTransitions == std::vector<bool>{true,false,true,false}));
    }
    assert(events.listeners<bedrock::UpdateEvent>() == 0);
    assert(events.listeners<bedrock::KeyEvent>() == 0);
}
