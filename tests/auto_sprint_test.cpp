#include <bedrock/client.hpp>
#include <cassert>
#include <iostream>
int main() {
    assert(bedrock::shouldSprint(true,true,false));
    assert(!bedrock::shouldSprint(true,false,false));
    assert(!bedrock::shouldSprint(false,true,false));
    assert(!bedrock::shouldSprint(true,true,true));
    assert(static_cast<unsigned>(bedrock::ModStatus::Unsupported)==3);
    std::cout << "AutoSprint SDK policy test: PASS\n";
}
