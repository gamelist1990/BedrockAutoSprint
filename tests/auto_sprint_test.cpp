#include "auto_sprint_policy.hpp"
#include <cassert>
#include <iostream>

int main() {
    using bedrock::shouldAutoSprint;
    assert(shouldAutoSprint(true,true,true,false));
    assert(!shouldAutoSprint(false,true,true,false));
    assert(!shouldAutoSprint(true,false,true,false));
    assert(!shouldAutoSprint(true,true,false,false));
    assert(!shouldAutoSprint(true,true,true,true));
    std::cout<<"auto_sprint_test: PASS\n";
}
